/*
  scout_firestore.ino
  Runs on the Arduino UNO R4 WiFi -- NO Raspberry Pi needed.

  What it does:
    - Connects to WiFi.
    - Signs in to Firebase Authentication (anonymous, or email/password).
    - Reads the 4 MQ gas sensors (new PCB pin mapping) + optional MLX90640
      thermal camera.
    - Sends the full 32x24 thermal frame with every reading as
      "thermal.pixels" (768 numbers in degrees C, row by row).
    - Writes one document per reading into Firestore (same schema the Pi
      used, so the dashboard keeps working) with a server-side timestamp
      in "server_time".df
    - Optionally captures an ArduCAM still every IMAGE_INTERVAL_MS, uploads
      it to Firebase Storage, and puts its URL in "image_url" on readings.
    - Optionally runs a tiny local web server (GET /reading, GET /image)
      so a device on the same WiFi can poll the board directly.

  ===========================================================================
  LIBRARIES TO INSTALL  (Arduino IDE -> Tools -> Manage Libraries...)
  ===========================================================================
    - ArduinoHttpClient      (by Arduino)
    - ArduinoJson            (by Benoit Blanchon) -- version 7.x
    - Adafruit MLX90640      (only if USE_THERMAL is 1; accept its dependencies)
    - ArduCAM                (only if USE_CAMERA is 1; install from ArduCAM's
                              GitHub, NOT in Library Manager. In its
                              memorysaver.h enable the line for your module,
                              e.g. OV2640_MINI_2MP)
    - WiFiS3                 (built in with the "Arduino UNO R4 Boards" package
                              -- Tools -> Board -> Boards Manager)

  Also: update the R4's WiFi firmware (Tools -> Firmware Updater in IDE 2.x).
  Old firmware has outdated HTTPS certificates and Google connections fail.

  ===========================================================================
  FIREBASE CONSOLE SETUP (one time)
  ===========================================================================
    1. Authentication -> Sign-in method -> enable "Anonymous"
       (or "Email/Password" if you fill in AUTH_EMAIL / AUTH_PASSWORD below
       -- then also create that user under Authentication -> Users).
    2. Project settings -> General -> copy the "Web API key" into
       FIREBASE_API_KEY below. (If there's no web API key yet, add a Web
       app to the project first.) If you've restricted that key to HTTP
       referrers in Google Cloud Console, the Arduino's requests will be
       rejected -- leave it unrestricted or allow these APIs.
    3. Firestore rules must allow authenticated creates in "readings".
    4. Storage rules must allow authenticated uploads to "images/"
       (only if UPLOAD_IMAGES_TO_STORAGE is 1).
       See the rules in the chat message that came with this file.

  Everything you need to change is in the "CHANGE ME" section below.
*/

// ===========================================================================
// ======================  CHANGE ME  ========================================
// ===========================================================================

// ---- Feature switches (1 = on, 0 = off) -----------------------------------
#define USE_THERMAL                1   // MLX90640 wired on I2C?
#define USE_CAMERA                 0   // ArduCAM wired on SPI (CS = pin 10)?
#define UPLOAD_IMAGES_TO_STORAGE   0   // upload stills to Firebase Storage (needs USE_CAMERA)
#define ENABLE_LOCAL_WEBSERVER     0   // serve /reading and /image on port 80

// ---- WiFi -------------------------------------------------------------------
// Must be a normal WPA2 password network (home router / phone hotspot).
// University/office "enterprise" WiFi (eduroam, username+password login,
// or a sign-in web page) will NOT work on the UNO R4 -- use a hotspot.
const char* WIFI_SSID = "Palantir Drone";        // <-- CHANGE ME
const char* WIFI_PASS = "12345678";    // <-- CHANGE ME

// ---- Firebase ---------------------------------------------------------------
const char* FIREBASE_API_KEY        = "AIzaSyCHz7t7BX5tgd6WU6Vva91U-XV594ykCuI";                   // <-- CHANGE ME (Project settings -> General)
const char* FIREBASE_PROJECT_ID     = "thermal-rover";                      // <-- CHANGE ME if different
const char* FIREBASE_STORAGE_BUCKET = "thermal-rover.firebasestorage.app";  // <-- CHANGE ME if different (Storage page, without gs://)
const char* READINGS_COLLECTION     = "readings";                           // Firestore collection the dashboard reads

// Leave both empty ("") to sign in anonymously.
// Fill both in to sign in as a specific Firebase email/password user instead.
const char* AUTH_EMAIL    = "";    // <-- optional
const char* AUTH_PASSWORD = "";    // <-- optional

// Name for this board. Letters, numbers and dashes only (it goes into URLs).
const char* DEVICE_ID = "scout-01";

// ---- Timing -----------------------------------------------------------------
// Firestore free tier = 20,000 writes/day. 5 s = 720 writes/hour.
const unsigned long PUBLISH_INTERVAL_MS = 5000;     // how often a reading is written
const unsigned long IMAGE_INTERVAL_MS   = 5000;    // how often a still is uploaded
const unsigned long WARMUP_MS           = 10000UL; // MQ heater warm-up (3 min). Set 0 to skip.

// ---- Location ---------------------------------------------------------------
// No GPS indoors. Set USE_FIXED_LOCATION true to stamp every reading with a
// fixed lat/lon (e.g. the demo room) so dashboard maps still show a pin.
// If false, lat/lon are written as null.
const bool   USE_FIXED_LOCATION = true;
const double FIXED_LAT = 53.7632;     // <-- CHANGE ME if used
const double FIXED_LON = -2.7031;     // <-- CHANGE ME if used

// ---- Thermal ----------------------------------------------------------------
const float HOTSPOT_THRESHOLD_C = 38.0;   // tune against the room's ambient temperature

// ---- Gas sensors ------------------------------------------------------------
// Circuit assumed on each sensor:   5V -- MQ -- AOUT -- RL -- GND
//   Rs = RL * (VCC / VOUT - 1)
//   ppm = A * (Rs / Ro)^B
//
// rlKOhm : load resistor on YOUR PCB for that sensor, in kOhm.  <-- CHECK ME
//          (look at the resistor next to each sensor / your PCB schematic)
// roKOhm : sensor resistance in clean air, in kOhm. Starting guesses only;
//          overwritten at boot if CALIBRATE_RO_AT_BOOT is true.
// A, B   : approximate datasheet curve fits.
// cleanAirRatio : datasheet Rs/Ro in clean air (used for boot calibration).
//
// All ppm values are rough estimates, not safety-grade measurements.
// MQ-2 is cross-sensitive (reported as LPG-equivalent). MQ-7 is only
// accurate with heater cycling, which this PCB doesn't do. MQ-135 is a
// general air-quality/VOC indicator -- treat it as relative.
const bool CALIBRATE_RO_AT_BOOT = true;   // true = measure Ro after warm-up. Only do this in clean air!

struct MQSensor {
  const char* label;      // for Serial output
  const char* key;        // field name in Firestore (matches the old dashboard schema)
  int   pin;
  float rlKOhm;           // <-- CHECK ME per your PCB
  float roKOhm;
  float curveA;
  float curveB;
  float cleanAirRatio;
};

// Pin mapping per the new PCB: A0 MQ-4, A1 MQ-7, A2 MQ-2, A3 MQ-135
MQSensor gasSensors[] = {
  //  label     key            pin  RL     Ro     A         B        clean-air Rs/Ro
  { "MQ-4",   "mq4_methane",  A0,  20.0f, 47.5f, 1012.7f,  -2.786f,  4.4f  },
  { "MQ-7",   "mq7_co",       A1,  10.0f, 10.0f,   99.042f, -1.518f, 27.5f  },
  { "MQ-2",   "mq2_smoke",    A2,   5.0f, 10.0f,  574.25f,  -2.222f,  9.83f },
  { "MQ-135", "mq135_voc",    A3,  10.0f, 10.0f,  110.47f,  -2.862f,  3.6f  },
};

// ===========================================================================
// ======================  END OF CHANGE ME  =================================
// ===========================================================================

#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <math.h>

#if USE_THERMAL
#include <Adafruit_MLX90640.h>
#endif

#if USE_CAMERA
#include <ArduCAM.h>
#endif

const int NUM_GAS = sizeof(gasSensors) / sizeof(gasSensors[0]);
const float GAS_VCC     = 5.0f;
const float GAS_ADC_MAX = 4095.0f;   // 12-bit ADC
const int   GAS_SAMPLES = 10;

const char* AUTH_HOST      = "identitytoolkit.googleapis.com";
const char* TOKEN_HOST     = "securetoken.googleapis.com";
const char* FIRESTORE_HOST = "firestore.googleapis.com";
const char* STORAGE_HOST   = "firebasestorage.googleapis.com";

const unsigned long TOKEN_REFRESH_MS = 50UL * 60UL * 1000UL;   // ID tokens last 60 min; refresh at 50

WiFiSSLClient ssl;

#if ENABLE_LOCAL_WEBSERVER
WiFiServer server(80);
#endif

#if USE_THERMAL
Adafruit_MLX90640 mlx;
const int THERM_W = 32;
const int THERM_H = 24;
float frame[THERM_W * THERM_H];
bool thermalOk = false;
bool frameValid = false;   // true when frame[] holds the frame for the reading being published
#endif

#if USE_CAMERA
#define ARDUCAM_CS_PIN 10
const uint32_t MAX_JPEG_BYTES = 500000;
ArduCAM myCAM(OV2640, ARDUCAM_CS_PIN);
bool cameraOk = false;
#endif

String idToken = "";
String refreshToken = "";
unsigned long tokenObtainedAt = 0;

String latestJson = "{}";
String latestImageUrl = "";

unsigned long seq = 0;
unsigned long lastPublish = 0;
unsigned long lastImage = 0;
unsigned long warmupStart = 0;
unsigned long lastWarmupPrint = 0;
bool warmupDone = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
String randomId(int n) {
  const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String s;
  s.reserve(n);
  for (int i = 0; i < n; i++) s += chars[random(62)];
  return s;
}

// POST a body over HTTPS. Returns HTTP status (or negative on connection error).
int httpsPost(const char* host, const String& path, const char* contentType,
              const String& body, const String& authHeader, String& responseOut) {
  HttpClient http(ssl, host, 443);
  http.setHttpResponseTimeout(15000);

  http.beginRequest();
  int err = http.post(path.c_str());
  if (err != 0) {
    http.stop();
    return -1;
  }
  http.sendHeader("Content-Type", contentType);
  http.sendHeader("Content-Length", (int)body.length());
  if (authHeader.length() > 0) http.sendHeader("Authorization", authHeader.c_str());
  http.beginBody();
  http.print(body);
  http.endRequest();

  int status = http.responseStatusCode();
  responseOut = http.responseBody();
  http.stop();
  return status;
}

#if USE_THERMAL
// ---------------------------------------------------------------------------
// Raw thermal frame -> Firestore
// The full frame is written as thermal.pixels: a flat array of 768 numbers in
// degrees C (1 decimal), row by row, so pixel (x, y) is pixels[y * 32 + x] --
// the same indexing as hotspot_px. It has to be flat because Firestore does
// not allow arrays inside arrays.
//
// As Firestore JSON that array is ~17 KB, too much to build in RAM on the R4,
// so the request body is built with a small placeholder where the array goes
// and the pixels are generated in chunks while the request is being sent.
// ---------------------------------------------------------------------------
const char* PIXELS_PLACEHOLDER = "{\"stringValue\":\"@PIXELS@\"}";
const char* PIXELS_OPEN  = "{\"arrayValue\":{\"values\":[";
const char* PIXELS_CLOSE = "]}}";

// Writes one pixel as a Firestore typed value into out. Returns its length.
int pixelToFirestore(float t, char* out, size_t outSize) {
  if (!isfinite(t)) return snprintf(out, outSize, "{\"nullValue\":null}");
  long tenths = lroundf(t * 10.0f);
  long mag = tenths < 0 ? -tenths : tenths;
  return snprintf(out, outSize, "{\"doubleValue\":%s%ld.%ld}",
                  tenths < 0 ? "-" : "", mag / 10, mag % 10);
}

// Like httpsPost, but sends the thermal frame in place of PIXELS_PLACEHOLDER
// (found at placeholderAt in body).
int httpsPostWithFrame(const char* host, const String& path, const String& body,
                       int placeholderAt, const String& authHeader, String& responseOut) {
  const int NUM_PX = THERM_W * THERM_H;
  const int placeholderLen = strlen(PIXELS_PLACEHOLDER);
  char px[40];

  // Content-Length has to be known before the body starts, so measure first.
  long pixelsLen = strlen(PIXELS_OPEN) + strlen(PIXELS_CLOSE) + (NUM_PX - 1);   // + commas
  for (int i = 0; i < NUM_PX; i++) pixelsLen += pixelToFirestore(frame[i], px, sizeof(px));

  HttpClient http(ssl, host, 443);
  http.setHttpResponseTimeout(15000);

  http.beginRequest();
  int err = http.post(path.c_str());
  if (err != 0) {
    http.stop();
    return -1;
  }
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", (int)(body.length() - placeholderLen + pixelsLen));
  if (authHeader.length() > 0) http.sendHeader("Authorization", authHeader.c_str());
  http.beginBody();

  const uint8_t* raw = (const uint8_t*)body.c_str();
  http.write(raw, placeholderAt);
  http.print(PIXELS_OPEN);

  // Batch pixels into 512-byte writes (one WiFi write per pixel is far too slow).
  char buf[512];
  size_t used = 0;
  for (int i = 0; i < NUM_PX; i++) {
    int n = pixelToFirestore(frame[i], px, sizeof(px));
    if (used + n + 1 > sizeof(buf)) {
      http.write((const uint8_t*)buf, used);
      used = 0;
    }
    if (i > 0) buf[used++] = ',';
    memcpy(buf + used, px, n);
    used += n;
  }
  if (used > 0) http.write((const uint8_t*)buf, used);

  http.print(PIXELS_CLOSE);
  int tailAt = placeholderAt + placeholderLen;
  http.write(raw + tailAt, body.length() - tailAt);
  http.endRequest();

  int status = http.responseStatusCode();
  responseOut = http.responseBody();
  http.stop();
  return status;
}
#endif

// ---------------------------------------------------------------------------
// Firebase Auth
// ---------------------------------------------------------------------------
bool firebaseSignIn() {
  bool useEmail = strlen(AUTH_EMAIL) > 0 && strlen(AUTH_PASSWORD) > 0;

  JsonDocument req;
  if (useEmail) {
    req["email"] = AUTH_EMAIL;
    req["password"] = AUTH_PASSWORD;
  }
  req["returnSecureToken"] = true;
  String body;
  serializeJson(req, body);

  String path = String("/v1/accounts:") + (useEmail ? "signInWithPassword" : "signUp") +
                "?key=" + FIREBASE_API_KEY;

  Serial.println(useEmail ? "[auth] signing in with email/password..." : "[auth] signing in anonymously...");
  String resp;
  int status = httpsPost(AUTH_HOST, path, "application/json", body, "", resp);
  if (status != 200) {
    Serial.print("[auth] FAILED, HTTP ");
    Serial.println(status);
    Serial.println(resp);
    Serial.println("[auth] Check FIREBASE_API_KEY and that the sign-in method is enabled in the console.");
    return false;
  }

  JsonDocument r;
  if (deserializeJson(r, resp)) {
    Serial.println("[auth] could not parse sign-in response");
    return false;
  }
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
  if (status != 200) {
    Serial.print("[auth] refresh failed, HTTP ");
    Serial.println(status);
    return false;
  }
  JsonDocument r;
  if (deserializeJson(r, resp)) return false;
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
// Plain JSON -> Firestore REST "typed value" JSON (recursive)
// e.g. 12 -> {"integerValue":"12"},  {"a":1} -> {"mapValue":{"fields":{...}}}
// Note: Firestore does NOT allow arrays inside arrays.
// ---------------------------------------------------------------------------
void toFirestoreValue(JsonVariantConst src, JsonObject dst) {
  if (src.isNull()) {
    dst["nullValue"] = nullptr;
  } else if (src.is<bool>()) {
    dst["booleanValue"] = src.as<bool>();
  } else if (src.is<long>()) {
    dst["integerValue"] = String(src.as<long>());
  } else if (src.is<unsigned long>()) {
    dst["integerValue"] = String(src.as<unsigned long>());
  } else if (src.is<double>()) {
    dst["doubleValue"] = src.as<double>();
  } else if (src.is<const char*>()) {
    dst["stringValue"] = src.as<const char*>();
  } else if (src.is<JsonObjectConst>()) {
    JsonObject fields = dst["mapValue"]["fields"].to<JsonObject>();
    for (JsonPairConst kv : src.as<JsonObjectConst>()) {
      toFirestoreValue(kv.value(), fields[kv.key()].to<JsonObject>());
    }
  } else if (src.is<JsonArrayConst>()) {
    JsonArray values = dst["arrayValue"]["values"].to<JsonArray>();
    for (JsonVariantConst item : src.as<JsonArrayConst>()) {
      toFirestoreValue(item, values.add<JsonObject>());
    }
  }
}

// ---------------------------------------------------------------------------
// Gas maths
// ---------------------------------------------------------------------------
int readGasAveraged(int pin) {
  long total = 0;
  for (int i = 0; i < GAS_SAMPLES; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / GAS_SAMPLES;
}

float rawToRsKOhm(int raw, float rlKOhm) {
  if (raw <= 0 || raw >= (int)GAS_ADC_MAX) return NAN;
  float v = ((float)raw * GAS_VCC) / GAS_ADC_MAX;
  if (v <= 0.001f || v >= GAS_VCC) return NAN;
  return rlKOhm * ((GAS_VCC / v) - 1.0f);
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
    float sum = 0;
    int good = 0;
    for (int k = 0; k < 20; k++) {
      float rs = rawToRsKOhm(readGasAveraged(gasSensors[i].pin), gasSensors[i].rlKOhm);
      if (isfinite(rs)) { sum += rs; good++; }
      delay(50);
    }
    if (good > 0) {
      gasSensors[i].roKOhm = (sum / good) / gasSensors[i].cleanAirRatio;
    }
    Serial.print("  ");
    Serial.print(gasSensors[i].label);
    Serial.print(" Ro = ");
    Serial.print(gasSensors[i].roKOhm, 2);
    Serial.println(good > 0 ? " kOhm" : " kOhm (calibration FAILED, kept default)");
  }
}

// ---------------------------------------------------------------------------
// Camera helpers
// ---------------------------------------------------------------------------
#if USE_CAMERA
uint32_t captureToFifo() {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();
  unsigned long t = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - t > 3000) return 0;
  }
  uint32_t len = myCAM.read_fifo_length();
  if (len == 0 || len > MAX_JPEG_BYTES) return 0;
  return len;
}

// Streams the captured JPEG in 512-byte chunks (much faster than 1 byte at a time over WiFi).
void streamFifoTo(Client& out, uint32_t len) {
  uint8_t buf[512];
  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  uint32_t remaining = len;
  while (remaining > 0) {
    size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    for (size_t i = 0; i < n; i++) buf[i] = SPI.transfer(0x00);
    out.write(buf, n);
    remaining -= n;
  }
  myCAM.CS_HIGH();
}
#endif

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.print("[wifi] connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println();
      Serial.println("[wifi] FAILED -- check WIFI_SSID / WIFI_PASS. Will retry.");
      return;
    }
  }
  Serial.println();

  IPAddress ip = WiFi.localIP();
  unsigned long ipStart = millis();
  while (ip == IPAddress(0, 0, 0, 0) && millis() - ipStart < 5000) {
    delay(300);
    ip = WiFi.localIP();
  }
  Serial.print("[wifi] connected, IP = ");
  Serial.println(ip);

#if ENABLE_LOCAL_WEBSERVER
  server.begin();
  Serial.print("[web] local server at http://");
  Serial.print(ip);
  Serial.println("/reading");
#endif
}

void ensureWiFi() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastAttempt != 0 && millis() - lastAttempt < 15000) return;
  lastAttempt = millis();
  connectWiFi();
}

// ---------------------------------------------------------------------------
// Build one reading (plain JSON, same schema the Pi used)
// ---------------------------------------------------------------------------
void buildReading(JsonDocument& doc) {
  doc["device_id"] = DEVICE_ID;
  doc["seq"] = seq;
  doc["millis"] = millis();

#if USE_THERMAL
  JsonObject thermal = doc["thermal"].to<JsonObject>();
  thermal["width"] = THERM_W;     // frontends check these against their own 32x24 and log a mismatch
  thermal["height"] = THERM_H;
  bool ok = thermalOk && (mlx.getFrame(frame) == 0);
  frameValid = ok;                // publishReading() adds thermal.pixels from frame[] when true
  if (ok) {
    float maxC = -999, sumC = 0;
    int hotspotCount = 0;
    JsonArray hp = thermal["hotspot_px"].to<JsonArray>();
    for (int i = 0; i < THERM_W * THERM_H; i++) {
      float t = frame[i];
      sumC += t;
      if (t > maxC) maxC = t;
      if (t > HOTSPOT_THRESHOLD_C) {
        hotspotCount++;
        if (hotspotCount <= 10) {         // only list the first 10 pixels
          JsonObject p = hp.add<JsonObject>();
          p["x"] = i % THERM_W;
          p["y"] = i / THERM_W;
        }
      }
    }
    thermal["max_c"] = round(maxC * 10) / 10.0;
    thermal["avg_c"] = round((sumC / (THERM_W * THERM_H)) * 10) / 10.0;
    thermal["hotspot_count"] = hotspotCount;
  } else {
    thermal["max_c"] = nullptr;
    thermal["avg_c"] = nullptr;
    thermal["hotspot_count"] = 0;
    thermal["hotspot_px"].to<JsonArray>();
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

  if (USE_FIXED_LOCATION) {
    doc["lat"] = FIXED_LAT;
    doc["lon"] = FIXED_LON;
  } else {
    doc["lat"] = nullptr;
    doc["lon"] = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Firestore publish
// ---------------------------------------------------------------------------
void publishReading() {
  seq++;

  JsonDocument reading;
  buildReading(reading);

  latestJson = "";
  serializeJson(reading, latestJson);
  Serial.println(latestJson);

  if (WiFi.status() != WL_CONNECTED) return;
  if (!ensureAuth()) return;

  // Build a Firestore "commit" request: create the document with a random ID
  // and set server_time to the server's clock (like SERVER_TIMESTAMP on the Pi).
  String docName = String("projects/") + FIREBASE_PROJECT_ID +
                   "/databases/(default)/documents/" + READINGS_COLLECTION + "/" + randomId(20);

  JsonDocument body;
  JsonObject write = body["writes"].add<JsonObject>();
  JsonObject update = write["update"].to<JsonObject>();
  update["name"] = docName;
  JsonObject fields = update["fields"].to<JsonObject>();
  for (JsonPairConst kv : reading.as<JsonObjectConst>()) {
    toFirestoreValue(kv.value(), fields[kv.key()].to<JsonObject>());
  }
  JsonObject transform = write["updateTransforms"].add<JsonObject>();
  transform["fieldPath"] = "server_time";
  transform["setToServerValue"] = "REQUEST_TIME";

#if USE_THERMAL
  // Reserve the spot for thermal.pixels; the 768 values are streamed in by
  // httpsPostWithFrame() instead of being held in this document.
  if (frameValid) fields["thermal"]["mapValue"]["fields"]["pixels"]["stringValue"] = "@PIXELS@";
#endif

  String bodyStr;
  serializeJson(body, bodyStr);

  String path = String("/v1/projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents:commit";
  String resp;
  int status;
#if USE_THERMAL
  int placeholderAt = frameValid ? bodyStr.indexOf(PIXELS_PLACEHOLDER) : -1;
  if (placeholderAt >= 0) {
    status = httpsPostWithFrame(FIRESTORE_HOST, path, bodyStr, placeholderAt, "Bearer " + idToken, resp);
  } else
#endif
  {
    status = httpsPost(FIRESTORE_HOST, path, "application/json", bodyStr, "Bearer " + idToken, resp);
  }

  if (status == 200) {
    Serial.print("[firestore] published seq=");
    Serial.println(seq);
  } else {
    Serial.print("[firestore] FAILED, HTTP ");
    Serial.println(status);
    Serial.println(resp.substring(0, 400));
    if (status == 401) idToken = "";   // token rejected -> sign in again next time
    if (status == 403) Serial.println("[firestore] 403 = permission denied. Check your Firestore rules.");
    if (status == 404) Serial.println("[firestore] 404 = check FIREBASE_PROJECT_ID and that Firestore is created.");
  }
}

// ---------------------------------------------------------------------------
// Firebase Storage image upload
// ---------------------------------------------------------------------------
#if USE_CAMERA && UPLOAD_IMAGES_TO_STORAGE
void captureAndUploadImage() {
  if (!cameraOk || WiFi.status() != WL_CONNECTED) return;
  if (!ensureAuth()) return;

  uint32_t len = captureToFifo();
  if (len == 0) {
    Serial.println("[image] capture failed");
    return;
  }

  String objectPath = String("images/") + DEVICE_ID + "_" + randomId(10) + ".jpg";
  String encoded = objectPath;
  encoded.replace("/", "%2F");
  String path = String("/v0/b/") + FIREBASE_STORAGE_BUCKET + "/o?uploadType=media&name=" + encoded;

  Serial.print("[image] uploading ");
  Serial.print(len);
  Serial.println(" bytes...");

  HttpClient http(ssl, STORAGE_HOST, 443);
  http.setHttpResponseTimeout(20000);
  http.beginRequest();
  if (http.post(path.c_str()) != 0) {
    Serial.println("[image] connection failed");
    http.stop();
    return;
  }
  http.sendHeader("Content-Type", "image/jpeg");
  http.sendHeader("Content-Length", (int)len);
  String auth = "Firebase " + idToken;
  http.sendHeader("Authorization", auth.c_str());
  http.beginBody();
  streamFifoTo(http, len);
  http.endRequest();

  int status = http.responseStatusCode();
  String resp = http.responseBody();
  http.stop();

  if (status != 200) {
    Serial.print("[image] upload FAILED, HTTP ");
    Serial.println(status);
    Serial.println(resp.substring(0, 400));
    if (status == 403) Serial.println("[image] 403 = check your Storage rules.");
    return;
  }

  JsonDocument filter;
  filter["downloadTokens"] = true;
  JsonDocument r;
  deserializeJson(r, resp, DeserializationOption::Filter(filter));
  String token = r["downloadTokens"] | "";
  int comma = token.indexOf(',');
  if (comma > 0) token = token.substring(0, comma);
  if (token.length() == 0) {
    Serial.println("[image] uploaded but no download token returned");
    return;
  }

  latestImageUrl = String("https://") + STORAGE_HOST + "/v0/b/" + FIREBASE_STORAGE_BUCKET +
                   "/o/" + encoded + "?alt=media&token=" + token;
  Serial.print("[image] OK: ");
  Serial.println(latestImageUrl);
}
#endif

// ---------------------------------------------------------------------------
// Local web server (optional)
// ---------------------------------------------------------------------------
#if ENABLE_LOCAL_WEBSERVER
void handleWebClient() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long waitStart = millis();
  while (client.connected() && !client.available()) {
    if (millis() - waitStart > 1000) { client.stop(); return; }
  }

  String requestLine = client.readStringUntil('\r');
  while (client.available()) {
    String headerLine = client.readStringUntil('\r');
    if (headerLine.length() <= 1) break;
  }

#if USE_CAMERA
  if (requestLine.indexOf("/image") >= 0) {
    uint32_t len = cameraOk ? captureToFifo() : 0;
    if (len == 0) {
      client.println("HTTP/1.1 503 Service Unavailable");
      client.println("Connection: close");
      client.println();
    } else {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: image/jpeg");
      client.println("Access-Control-Allow-Origin: *");
      client.print("Content-Length: ");
      client.println(len);
      client.println("Connection: close");
      client.println();
      streamFifoTo(client, len);
    }
    client.flush();
    delay(10);
    client.stop();
    return;
  }
#endif

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.print("Content-Length: ");
  client.println(latestJson.length());
  client.println("Connection: close");
  client.println();
  client.print(latestJson);
  client.flush();
  delay(10);
  client.stop();
}
#endif

// ---------------------------------------------------------------------------
// Setup / self-test
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) { ; }   // don't hang if running on battery with no USB

  analogReadResolution(12);

#if USE_THERMAL
  Wire.begin();
  if (mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_4_HZ);
    thermalOk = true;
    Serial.println("[thermal] MLX90640 found");
  } else {
    Serial.println("[thermal] MLX90640 NOT found -- check wiring. Continuing without it.");
  }
#endif

#if USE_CAMERA
  #if !USE_THERMAL
  Wire.begin();
  #endif
  SPI.begin();
  pinMode(ARDUCAM_CS_PIN, OUTPUT);
  digitalWrite(ARDUCAM_CS_PIN, HIGH);
  myCAM.write_reg(0x07, 0x80);
  delay(100);
  myCAM.write_reg(0x07, 0x00);
  delay(100);

  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
  if (myCAM.read_reg(ARDUCHIP_TEST1) == 0x55) {
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(OV2640_320x240);
    delay(500);
    uint32_t len = captureToFifo();
    cameraOk = len > 0;
    Serial.print("[camera] ");
    Serial.println(cameraOk ? "test capture OK, bytes=" + String(len) : String("test capture FAILED"));
  } else {
    Serial.println("[camera] SPI test FAILED -- check wiring. Continuing without camera.");
  }
#endif

  String fv = WiFi.firmwareVersion();
  Serial.print("[wifi] module firmware ");
  Serial.println(fv);
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("[wifi] WARNING: firmware is out of date -- update it if HTTPS fails.");
  }

  connectWiFi();

  randomSeed(micros() ^ ((unsigned long)analogRead(A0) << 12) ^ analogRead(A3));

  if (WiFi.status() == WL_CONNECTED) ensureAuth();

  warmupStart = millis();
  if (WARMUP_MS == 0) {
    warmupDone = true;
  } else {
    Serial.println("[gas] warming up sensors...");
  }
}

void handleWarmup() {
  unsigned long elapsed = millis() - warmupStart;
  if (elapsed >= WARMUP_MS) {
    warmupDone = true;
    Serial.println("[gas] warm-up complete");
    if (CALIBRATE_RO_AT_BOOT) calibrateRo();
    lastPublish = millis() - PUBLISH_INTERVAL_MS;   // publish straight away
    lastImage = millis() - IMAGE_INTERVAL_MS;       // and grab an image straight away
    return;
  }
  if (lastWarmupPrint == 0 || elapsed - lastWarmupPrint >= 10000) {
    lastWarmupPrint = elapsed == 0 ? 1 : elapsed;
    Serial.print("[gas] warm-up ");
    Serial.print((WARMUP_MS - elapsed) / 1000);
    Serial.print("s left --");
    for (int i = 0; i < NUM_GAS; i++) {
      Serial.print(" ");
      Serial.print(gasSensors[i].label);
      Serial.print("=");
      Serial.print(analogRead(gasSensors[i].pin));
    }
    Serial.println();
  }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void loop() {
  ensureWiFi();

#if ENABLE_LOCAL_WEBSERVER
  if (WiFi.status() == WL_CONNECTED) handleWebClient();
#endif

  if (!warmupDone) {
    handleWarmup();
    return;
  }

  unsigned long now = millis();

#if USE_CAMERA && UPLOAD_IMAGES_TO_STORAGE
  if (now - lastImage >= IMAGE_INTERVAL_MS) {
    lastImage = now;
    captureAndUploadImage();
  }
#endif

  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = millis();
    publishReading();
  }
}