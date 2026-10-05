/*
  scout_lean.ino  --  Arduino UNO R4 WiFi
  Gas sensors + MLX90640 thermal camera -> Firestore (+ optional thermal
  heat-map pictures -> Firebase Storage).

  This version is built to use as little RAM as possible (the R4 only has 32 KB):
    - thermal frame is read in loop(), not deep inside the JSON code
    - JSON documents are freed before every HTTPS send
    - every HTTPS request gets its own fresh connection
    - request bodies are sent in small chunks
    - thermal pictures are generated row-by-row while uploading (no image buffer)
    - prints [thermal], [http] and [mem] lines so we can see exactly where it stops

  Libraries: ArduinoHttpClient, ArduinoJson 7.x, Adafruit MLX90640.

  FIRST RUN: leave UPLOAD_THERMAL_IMAGES at 0. Once readings publish steadily
  (seq 5, 6, 7...), set it to 1.
*/

// ======================  CHANGE ME  ========================================
#define USE_THERMAL            1   // MLX90640 on I2C
#define UPLOAD_THERMAL_IMAGES  0   // heat-map pictures to Storage -- turn on after readings are stable

const char* WIFI_SSID = "DIGICEL-0D4A";
const char* WIFI_PASS = "AMYBR3ERAQD11";

const char* FIREBASE_API_KEY        = "AIzaSyCHz7t7BX5tgd6WU6Vva91U-XV594ykCuI";
const char* FIREBASE_PROJECT_ID     = "thermal-rover";
const char* FIREBASE_STORAGE_BUCKET = "thermal-rover.firebasestorage.app";
const char* READINGS_COLLECTION     = "readings";
const char* DEVICE_ID               = "scout-01";

const unsigned long PUBLISH_INTERVAL_MS       = 5000;
const unsigned long THERMAL_IMAGE_INTERVAL_MS = 30000;
const unsigned long WARMUP_MS                 = 10000UL;   // 180000UL (3 min) before trusting gas ppm

const bool   USE_FIXED_LOCATION = true;
const double FIXED_LAT = 53.7632;    // <-- change to your demo site
const double FIXED_LON = -2.7031;

const float HOTSPOT_THRESHOLD_C = 38.0;
const float THERMAL_MIN_SPAN_C  = 5.0;
#define THERMAL_FLIP_X 0

const bool CALIBRATE_RO_AT_BOOT = true;

struct MQSensor {
  const char* label; const char* key; int pin;
  float rlKOhm, roKOhm, curveA, curveB, cleanAirRatio;
};
MQSensor gasSensors[] = {
  { "MQ-4",   "mq4_methane", A0, 20.0f, 47.5f, 1012.7f,  -2.786f,  4.4f  },
  { "MQ-7",   "mq7_co",      A1, 10.0f, 10.0f,   99.042f, -1.518f, 27.5f  },
  { "MQ-2",   "mq2_smoke",   A2,  5.0f, 10.0f,  574.25f,  -2.222f,  9.83f },
  { "MQ-135", "mq135_voc",   A3, 10.0f, 10.0f,  110.47f,  -2.862f,  3.6f  },
};
// ======================  END CHANGE ME  ====================================

#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#if USE_THERMAL
#include <Adafruit_MLX90640.h>
#endif

const int   NUM_GAS     = sizeof(gasSensors) / sizeof(gasSensors[0]);
const float GAS_VCC     = 5.0f;
const float GAS_ADC_MAX = 4095.0f;
const int   GAS_SAMPLES = 10;

const char* AUTH_HOST      = "identitytoolkit.googleapis.com";
const char* TOKEN_HOST     = "securetoken.googleapis.com";
const char* FIRESTORE_HOST = "firestore.googleapis.com";
const char* STORAGE_HOST   = "firebasestorage.googleapis.com";
const unsigned long TOKEN_REFRESH_MS = 50UL * 60UL * 1000UL;

#if USE_THERMAL
#define THERM_W 32
#define THERM_H 24
Adafruit_MLX90640 mlx;
float frame[THERM_W * THERM_H];
bool  thermalOk = false;
bool  thermalFrameValid = false;
float thermMax = 0, thermAvg = 0;
int   thermHotCount = 0;
uint8_t hotX[10], hotY[10];
int   hotListed = 0;
unsigned long lastThermalImage = 0;
#endif

String idToken, refreshToken;
unsigned long tokenObtainedAt = 0;
String latestImageUrl;

unsigned long seq = 0, lastPublish = 0, warmupStart = 0, lastWarmupPrint = 0;
bool warmupDone = false;

// ---------------------------------------------------------------------------
extern "C" char* sbrk(int incr);
int freeRam() { char top; return &top - reinterpret_cast<char*>(sbrk(0)); }
void printMem(const char* where) {
  Serial.print("[mem] "); Serial.print(where); Serial.print(": "); Serial.println(freeRam());
}

String randomId(int n) {
  const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String s; s.reserve(n);
  for (int i = 0; i < n; i++) s += chars[random(62)];
  return s;
}

// ---------------------------------------------------------------------------
// HTTPS POST: fresh connection each time, body sent in 256-byte chunks.
// ---------------------------------------------------------------------------
int httpsPost(const char* host, const String& path, const char* contentType,
              const String& body, const String& authHeader, String& responseOut) {
  WiFiSSLClient client;
  HttpClient http(client, host, 443);
  http.setHttpResponseTimeout(15000);

  Serial.print("[http] connect "); Serial.println(host);
  http.beginRequest();
  if (http.post(path.c_str()) != 0) {
    Serial.println("[http] connect failed");
    http.stop(); client.stop();
    return -1;
  }
  http.sendHeader("Content-Type", contentType);
  http.sendHeader("Content-Length", (int)body.length());
  if (authHeader.length() > 0) http.sendHeader("Authorization", authHeader.c_str());
  http.beginBody();

  const uint8_t* p = (const uint8_t*)body.c_str();
  size_t len = body.length(), sent = 0;
  while (sent < len) {
    size_t n = (len - sent) > 256 ? 256 : (len - sent);
    size_t w = http.write(p + sent, n);
    if (w == 0) { Serial.println("[http] write stalled"); break; }
    sent += w;
    delay(2);
  }
  Serial.print("[http] sent "); Serial.print(sent); Serial.print("/"); Serial.println(len);
  http.endRequest();

  Serial.println("[http] waiting for status");
  int status = http.responseStatusCode();
  Serial.print("[http] status "); Serial.println(status);
  responseOut = (status > 0) ? http.responseBody() : String("");
  http.stop(); client.stop();
  delay(50);
  return status;
}

// ---------------------------------------------------------------------------
// Firebase Auth
// ---------------------------------------------------------------------------
bool firebaseSignIn() {
  String path = String("/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  Serial.println("[auth] signing in anonymously...");
  String resp;
  int status = httpsPost(AUTH_HOST, path, "application/json", "{\"returnSecureToken\":true}", "", resp);
  if (status != 200) {
    Serial.print("[auth] FAILED, HTTP "); Serial.println(status);
    Serial.println(resp.substring(0, 300));
    return false;
  }
  JsonDocument filter;
  filter["idToken"] = true;
  filter["refreshToken"] = true;
  JsonDocument r;
  if (deserializeJson(r, resp, DeserializationOption::Filter(filter))) return false;
  idToken = r["idToken"].as<String>();
  refreshToken = r["refreshToken"].as<String>();
  tokenObtainedAt = millis();
  Serial.println("[auth] OK");
  return idToken.length() > 0;
}

bool firebaseRefresh() {
  if (refreshToken.length() == 0) return false;
  String path = String("/v1/token?key=") + FIREBASE_API_KEY;
  String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
  String resp;
  int status = httpsPost(TOKEN_HOST, path, "application/x-www-form-urlencoded", body, "", resp);
  if (status != 200) return false;
  JsonDocument filter;
  filter["id_token"] = true;
  filter["refresh_token"] = true;
  JsonDocument r;
  if (deserializeJson(r, resp, DeserializationOption::Filter(filter))) return false;
  idToken = r["id_token"].as<String>();
  refreshToken = r["refresh_token"].as<String>();
  tokenObtainedAt = millis();
  Serial.println("[auth] token refreshed");
  return idToken.length() > 0;
}

bool ensureAuth() {
  if (idToken.length() == 0) return firebaseSignIn();
  if (millis() - tokenObtainedAt > TOKEN_REFRESH_MS) {
    if (!firebaseRefresh()) return firebaseSignIn();
  }
  return true;
}

// ---------------------------------------------------------------------------
// JSON -> Firestore typed values
// ---------------------------------------------------------------------------
void toFirestoreValue(JsonVariantConst src, JsonObject dst) {
  if (src.isNull())                   dst["nullValue"] = nullptr;
  else if (src.is<bool>())            dst["booleanValue"] = src.as<bool>();
  else if (src.is<long>())            dst["integerValue"] = String(src.as<long>());
  else if (src.is<unsigned long>())   dst["integerValue"] = String(src.as<unsigned long>());
  else if (src.is<double>())          dst["doubleValue"] = src.as<double>();
  else if (src.is<const char*>())     dst["stringValue"] = src.as<const char*>();
  else if (src.is<JsonObjectConst>()) {
    JsonObject fields = dst["mapValue"]["fields"].to<JsonObject>();
    for (JsonPairConst kv : src.as<JsonObjectConst>())
      toFirestoreValue(kv.value(), fields[kv.key()].to<JsonObject>());
  } else if (src.is<JsonArrayConst>()) {
    JsonArray values = dst["arrayValue"]["values"].to<JsonArray>();
    for (JsonVariantConst item : src.as<JsonArrayConst>())
      toFirestoreValue(item, values.add<JsonObject>());
  }
}

// ---------------------------------------------------------------------------
// Gas
// ---------------------------------------------------------------------------
int readGasAveraged(int pin) {
  long total = 0;
  for (int i = 0; i < GAS_SAMPLES; i++) { total += analogRead(pin); delay(2); }
  return total / GAS_SAMPLES;
}

float rawToRsKOhm(int raw, float rl) {
  if (raw <= 0 || raw >= (int)GAS_ADC_MAX) return NAN;
  float v = ((float)raw * GAS_VCC) / GAS_ADC_MAX;
  if (v <= 0.001f || v >= GAS_VCC) return NAN;
  return rl * ((GAS_VCC / v) - 1.0f);
}

float rawToPPM(const MQSensor& s, int raw) {
  float rs = rawToRsKOhm(raw, s.rlKOhm);
  if (!isfinite(rs) || rs <= 0.0f || s.roKOhm <= 0.0f) return NAN;
  float ppm = s.curveA * pow(rs / s.roKOhm, s.curveB);
  if (!isfinite(ppm) || ppm < 0.0f || ppm > 1000000.0f) return NAN;
  return ppm;
}

void calibrateRo() {
  Serial.println("[gas] calibrating Ro in clean air...");
  for (int i = 0; i < NUM_GAS; i++) {
    float sum = 0; int good = 0;
    for (int k = 0; k < 20; k++) {
      float rs = rawToRsKOhm(readGasAveraged(gasSensors[i].pin), gasSensors[i].rlKOhm);
      if (isfinite(rs)) { sum += rs; good++; }
      delay(50);
    }
    if (good > 0) gasSensors[i].roKOhm = (sum / good) / gasSensors[i].cleanAirRatio;
    Serial.print("  "); Serial.print(gasSensors[i].label);
    Serial.print(" Ro = "); Serial.println(gasSensors[i].roKOhm, 2);
  }
}

// ---------------------------------------------------------------------------
// Thermal: read + summarise (called from loop, shallow stack)
// ---------------------------------------------------------------------------
#if USE_THERMAL
void readThermal() {
  thermalFrameValid = false;
  if (!thermalOk) return;

  Serial.println("[thermal] reading frame...");
  int rc = mlx.getFrame(frame);
  if (rc != 0) {
    Serial.print("[thermal] getFrame error "); Serial.println(rc);
    return;
  }

  float maxC = -999, sumC = 0;
  int valid = 0;
  thermHotCount = 0;
  hotListed = 0;
  for (int i = 0; i < THERM_W * THERM_H; i++) {
    float t = frame[i];
    if (!isfinite(t)) { frame[i] = 0; continue; }
    sumC += t; valid++;
    if (t > maxC) maxC = t;
    if (t > HOTSPOT_THRESHOLD_C) {
      thermHotCount++;
      if (hotListed < 10) { hotX[hotListed] = i % THERM_W; hotY[hotListed] = i / THERM_W; hotListed++; }
    }
  }
  if (valid == 0) { Serial.println("[thermal] frame had no valid pixels"); return; }
  thermMax = round(maxC * 10) / 10.0;
  thermAvg = round((sumC / valid) * 10) / 10.0;
  thermalFrameValid = true;
  Serial.print("[thermal] frame OK, max "); Serial.println(thermMax, 1);
}
#endif

// ---------------------------------------------------------------------------
// Build + publish one reading
// ---------------------------------------------------------------------------
void buildReading(JsonDocument& doc) {
  doc["device_id"] = DEVICE_ID;
  doc["seq"] = seq;
  doc["millis"] = millis();

#if USE_THERMAL
  JsonObject thermal = doc["thermal"].to<JsonObject>();
  thermal["width"] = THERM_W;     // frontends check these against their own 32x24 and log a mismatch
  thermal["height"] = THERM_H;
  JsonArray hp = thermal["hotspot_px"].to<JsonArray>();
  if (thermalFrameValid) {
    thermal["max_c"] = thermMax;
    thermal["avg_c"] = thermAvg;
    thermal["hotspot_count"] = thermHotCount;
    for (int i = 0; i < hotListed; i++) {
      JsonObject p = hp.add<JsonObject>();
      p["x"] = hotX[i];
      p["y"] = hotY[i];
    }
  } else {
    thermal["max_c"] = nullptr;
    thermal["avg_c"] = nullptr;
    thermal["hotspot_count"] = 0;
  }
#endif

  JsonObject gasPpm = doc["gas_ppm"].to<JsonObject>();
  JsonObject gasRaw = doc["gas_raw"].to<JsonObject>();
  for (int i = 0; i < NUM_GAS; i++) {
    int raw = readGasAveraged(gasSensors[i].pin);
    float ppm = rawToPPM(gasSensors[i], raw);
    gasRaw[gasSensors[i].key] = raw;
    if (isfinite(ppm)) gasPpm[gasSensors[i].key] = (long)lroundf(ppm);
    else               gasPpm[gasSensors[i].key] = nullptr;
  }

  doc["image_available"] = latestImageUrl.length() > 0;
  if (latestImageUrl.length() > 0) doc["image_url"] = latestImageUrl;
  else                             doc["image_url"] = nullptr;

  if (USE_FIXED_LOCATION) { doc["lat"] = FIXED_LAT; doc["lon"] = FIXED_LON; }
  else                    { doc["lat"] = nullptr;   doc["lon"] = nullptr; }
}

void publishReading() {
  seq++;
  String bodyStr;

  {   // both JSON documents are freed at the closing brace, before sending
    JsonDocument reading;
    buildReading(reading);
    serializeJson(reading, Serial);
    Serial.println();

    JsonDocument body;
    JsonObject write = body["writes"].add<JsonObject>();
    JsonObject update = write["update"].to<JsonObject>();
    update["name"] = String("projects/") + FIREBASE_PROJECT_ID +
                     "/databases/(default)/documents/" + READINGS_COLLECTION + "/" + randomId(20);
    JsonObject fields = update["fields"].to<JsonObject>();
    for (JsonPairConst kv : reading.as<JsonObjectConst>())
      toFirestoreValue(kv.value(), fields[kv.key()].to<JsonObject>());
    JsonObject transform = write["updateTransforms"].add<JsonObject>();
    transform["fieldPath"] = "server_time";
    transform["setToServerValue"] = "REQUEST_TIME";

    serializeJson(body, bodyStr);
  }

  if (WiFi.status() != WL_CONNECTED) return;
  if (!ensureAuth()) return;

  printMem("before firestore send");
  String path = String("/v1/projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents:commit";
  String resp;
  int status = httpsPost(FIRESTORE_HOST, path, "application/json", bodyStr, "Bearer " + idToken, resp);

  if (status == 200) {
    Serial.print("[firestore] published seq="); Serial.println(seq);
  } else {
    Serial.print("[firestore] FAILED, HTTP "); Serial.println(status);
    Serial.println(resp.substring(0, 300));
    if (status == 401) idToken = "";
  }
}

// ---------------------------------------------------------------------------
// Thermal picture: 32x24 BMP streamed row-by-row (no image buffer in RAM)
// ---------------------------------------------------------------------------
#if USE_THERMAL && UPLOAD_THERMAL_IMAGES
#define BMP_HEADER_BYTES 54
#define BMP_ROW_BYTES    (THERM_W * 3)                              // 96, already a multiple of 4
#define BMP_TOTAL_BYTES  (BMP_HEADER_BYTES + BMP_ROW_BYTES * THERM_H) // 2358

const uint8_t PALETTE[][3] = {
  {0,0,0}, {40,0,110}, {150,0,150}, {230,60,20}, {255,170,0}, {255,240,80}, {255,255,255},
};
const int PALETTE_STOPS = sizeof(PALETTE) / sizeof(PALETTE[0]);

void heatColor(float t, uint8_t* rgb) {
  if (!(t >= 0)) t = 0;   // also catches NaN
  if (t > 1) t = 1;
  float pos = t * (PALETTE_STOPS - 1);
  int i = (int)pos;
  if (i >= PALETTE_STOPS - 1) i = PALETTE_STOPS - 2;
  float f = pos - i;
  for (int c = 0; c < 3; c++)
    rgb[c] = (uint8_t)(PALETTE[i][c] + (PALETTE[i + 1][c] - PALETTE[i][c]) * f);
}

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

void uploadThermalImage() {
  if (!thermalFrameValid || WiFi.status() != WL_CONNECTED) return;
  if (!ensureAuth()) return;

  float lo = frame[0], hi = frame[0];
  for (int i = 1; i < THERM_W * THERM_H; i++) {
    if (frame[i] < lo) lo = frame[i];
    if (frame[i] > hi) hi = frame[i];
  }
  if (hi - lo < THERMAL_MIN_SPAN_C) hi = lo + THERMAL_MIN_SPAN_C;

  String encoded = String("images%2F") + DEVICE_ID + "_thermal_" + randomId(10) + ".bmp";
  String path = String("/v0/b/") + FIREBASE_STORAGE_BUCKET + "/o?uploadType=media&name=" + encoded;
  String auth = "Firebase " + idToken;

  printMem("before image upload");
  Serial.println("[thermal-img] uploading...");

  WiFiSSLClient client;
  HttpClient http(client, STORAGE_HOST, 443);
  http.setHttpResponseTimeout(15000);
  http.beginRequest();
  if (http.post(path.c_str()) != 0) {
    Serial.println("[thermal-img] connect failed");
    http.stop(); client.stop();
    return;
  }
  http.sendHeader("Content-Type", "image/bmp");
  http.sendHeader("Content-Length", BMP_TOTAL_BYTES);
  http.sendHeader("Authorization", auth.c_str());
  http.beginBody();

  uint8_t hdr[BMP_HEADER_BYTES] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  put32(hdr + 2,  BMP_TOTAL_BYTES);
  put32(hdr + 10, BMP_HEADER_BYTES);
  put32(hdr + 14, 40);
  put32(hdr + 18, THERM_W);
  put32(hdr + 22, THERM_H);
  put16(hdr + 26, 1);
  put16(hdr + 28, 24);
  put32(hdr + 34, BMP_ROW_BYTES * THERM_H);
  http.write(hdr, BMP_HEADER_BYTES);

  uint8_t row[BMP_ROW_BYTES];
  uint8_t rgb[3];
  for (int r = 0; r < THERM_H; r++) {
    int y = THERM_H - 1 - r;   // BMP stores bottom row first
    uint8_t* px = row;
    for (int col = 0; col < THERM_W; col++) {
      int x = THERMAL_FLIP_X ? (THERM_W - 1 - col) : col;
      heatColor((frame[y * THERM_W + x] - lo) / (hi - lo), rgb);
      *px++ = rgb[2]; *px++ = rgb[1]; *px++ = rgb[0];   // B, G, R
    }
    http.write(row, BMP_ROW_BYTES);
    delay(2);
  }
  http.endRequest();

  int status = http.responseStatusCode();
  Serial.print("[thermal-img] status "); Serial.println(status);
  String resp = (status > 0) ? http.responseBody() : String("");
  http.stop(); client.stop();
  delay(50);

  if (status != 200) {
    Serial.println(resp.substring(0, 300));
    if (status == 401) idToken = "";
    if (status == 403) Serial.println("[thermal-img] 403 = Storage rules must allow image/bmp in images/");
    return;
  }

  JsonDocument filter;
  filter["downloadTokens"] = true;
  JsonDocument r;
  if (deserializeJson(r, resp, DeserializationOption::Filter(filter))) return;
  String token = r["downloadTokens"] | "";
  int comma = token.indexOf(',');
  if (comma > 0) token = token.substring(0, comma);
  if (token.length() == 0) { Serial.println("[thermal-img] no download token"); return; }

  latestImageUrl = String("https://") + STORAGE_HOST + "/v0/b/" + FIREBASE_STORAGE_BUCKET +
                   "/o/" + encoded + "?alt=media&token=" + token;
  Serial.print("[thermal-img] OK: "); Serial.println(latestImageUrl);
}
#endif

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.print("[wifi] connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (millis() - start > 20000) { Serial.println("\n[wifi] FAILED, will retry"); return; }
  }
  unsigned long ipStart = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - ipStart < 5000) delay(300);
  Serial.print("\n[wifi] connected, IP = "); Serial.println(WiFi.localIP());
}

void ensureWiFi() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastAttempt != 0 && millis() - lastAttempt < 15000) return;
  lastAttempt = millis();
  connectWiFi();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) { ; }
  analogReadResolution(12);

#if USE_THERMAL
  Wire.begin();
  Wire.setClock(400000);
  if (mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_2_HZ);
    thermalOk = true;
    Serial.println("[thermal] MLX90640 found");
  } else {
    Serial.println("[thermal] MLX90640 NOT found -- check wiring");
  }
#endif

  Serial.print("[wifi] module firmware "); Serial.println(WiFi.firmwareVersion());
  connectWiFi();
  randomSeed(micros() ^ ((unsigned long)analogRead(A0) << 12) ^ analogRead(A3));
  if (WiFi.status() == WL_CONNECTED) ensureAuth();

  printMem("after setup");
  warmupStart = millis();
  warmupDone = (WARMUP_MS == 0);
  if (!warmupDone) Serial.println("[gas] warming up sensors...");
}

void loop() {
  ensureWiFi();

  if (!warmupDone) {
    unsigned long elapsed = millis() - warmupStart;
    if (elapsed >= WARMUP_MS) {
      warmupDone = true;
      Serial.println("[gas] warm-up complete");
      if (CALIBRATE_RO_AT_BOOT) calibrateRo();
      lastPublish = millis() - PUBLISH_INTERVAL_MS;
    } else if (lastWarmupPrint == 0 || elapsed - lastWarmupPrint >= 10000) {
      lastWarmupPrint = elapsed ? elapsed : 1;
      Serial.print("[gas] warm-up "); Serial.print((WARMUP_MS - elapsed) / 1000); Serial.println("s left");
    }
    return;
  }

  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = millis();
    Serial.println("---------------- cycle ----------------");
#if USE_THERMAL
    readThermal();
#endif
    publishReading();

#if USE_THERMAL && UPLOAD_THERMAL_IMAGES
    if (thermalFrameValid &&
        (lastThermalImage == 0 || millis() - lastThermalImage >= THERMAL_IMAGE_INTERVAL_MS)) {
      lastThermalImage = millis();
      uploadThermalImage();
    }
#endif
  }
}
