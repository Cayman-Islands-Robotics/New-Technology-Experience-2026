/*
  scout_sensor_hub.ino
  Runs on the Arduino UNO R4 WiFi.

  Reads the MLX90640 thermal camera + 2 MQ gas sensors (analog), packages
  each reading as one line of JSON over USB serial -- matching the schema
  pi_sensor_publisher.py expects. Periodically triggers an Arducam still
  capture and streams the raw JPEG bytes over the same serial link, wrapped
  in header/footer JSON markers so the Pi can tell where the image starts
  and ends.

  Also runs a tiny WiFi HTTP server so the dashboard can poll this board
  directly (GET /reading -> latest JSON, GET /image -> live JPEG) when the
  Pi isn't in the loop yet -- e.g. for a demo on a phone hotspot.

  Libraries needed (Arduino IDE -> Tools -> Manage Libraries):
    - Adafruit MLX90640
    - ArduinoJson
    - ArduCAM (install per ArduCAM's own GitHub instructions -- their library
      isn't in the standard Library Manager index)
    - WiFiS3 (built in to the UNO R4 WiFi board package)

  NOTE: this is a starting sketch, not a finished one. A few things are
  placeholders you'll need to tune on the bench -- flagged with comments
  below. Exact API calls for Adafruit_MLX90640 and ArduCAM can vary a bit
  by library version, so double check function names against whatever
  version you actually install.

  Wiring: matches the wiring chart from earlier -- shared I2C bus (SDA/SCL)
  for MLX90640 + Arducam, SPI pins 10-13 to Arducam, A0/A1 to the 2 gas
  sensors currently wired (MQ-2 smoke, MQ-4 methane).

  On boot this sketch:
    1. Connects to WiFi and starts the local web server.
    2. Runs a self-test -- confirms the thermal camera responds and does
       one test capture with the ArduCAM to confirm the SPI image path
       works end to end.
    3. Runs a ~3 minute warm-up period (gas sensor heaters need this to
       give stable readings) before starting normal telemetry.
*/

#include <Wire.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Adafruit_MLX90640.h>
#include <ArduCAM.h>
#include <WiFiS3.h>
#include <math.h>

// ---------------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------------
#define ARDUCAM_CS_PIN 10
const unsigned long TELEMETRY_INTERVAL_MS = 1000;    // send sensor JSON once per second
const unsigned long IMAGE_INTERVAL_MS     = 15000;   // capture + send a still every 15s
const unsigned long WARMUP_MS             = 180000UL; // 3 minutes gas-sensor warm-up
const float HOTSPOT_THRESHOLD_C = 38.0;              // PLACEHOLDER -- tune once you know ambient baseline at the dump

// PLACEHOLDER -- fill in for tonight's demo
const char* WIFI_SSID = "Palantir Drone";
const char* WIFI_PASS = "12345678";

// Static IP so the Arduino always gets the same address on this hotspot.
// Use the IP that already worked for you as local_IP. Gateway is almost
// always the .1 address on the same subnet (check your phone's hotspot
// info screen if unsure). Subnet 255.255.255.0 is correct for the vast
// majority of phone hotspots.
IPAddress local_IP(192, 168, 43, 50);   // <-- set to an IP on your hotspot's subnet, not already in use
IPAddress gateway(192, 168, 43, 1);     // <-- your hotspot's gateway (usually .1)
IPAddress subnet(255, 255, 255, 0);

WiFiServer server(80);
String latestJson = "{}";

Adafruit_MLX90640 mlx;
ArduCAM myCAM(OV2640, ARDUCAM_CS_PIN);

float frame[32 * 24];       // raw thermal frame buffer (32x24 pixels)
unsigned long lastTelemetry = 0;
unsigned long lastImage = 0;
unsigned long seq = 0;

// ---------------------------------------------------------------------------
// MQ gas conversion
// ---------------------------------------------------------------------------
//
// Assumed analogue circuit:
//
//   5V -- MQ sensor -- AOUT -- RL -- GND
//
// For that arrangement:
//
//   Rs = RL x (VCC / VOUT - 1)
//
// The values below are fixed nominal assumptions. No startup calibration is
// performed.
//
// IMPORTANT:
// - MQ-2 is cross-sensitive to LPG, methane, hydrogen, alcohol vapour, smoke,
//   propane and other gases. Its result is labelled LPG-equivalent.
// - MQ-4 is intended primarily for methane.
// - Both results are approximate and not safety-certified measurements.
// ---------------------------------------------------------------------------

const float GAS_VCC = 5.0f;
const float GAS_ADC_MAX = 4095.0f;  // 12-bit ADC because analogReadResolution(12)

// Approximate load resistors in kOhm.
const float MQ2_RL_KOHM = 5.0f;
const float MQ4_RL_KOHM = 20.0f;

// Fixed nominal baseline sensor resistances in kOhm.
const float MQ2_RO_KOHM = 10.0f;
const float MQ4_RO_KOHM = 47.5f;

// Approximate curves:
//
//   ppm = A x pow(Rs / Ro, B)
//
// MQ-2: LPG-equivalent approximation.
const float MQ2_LPG_A = 574.25f;
const float MQ2_LPG_B = -2.222f;

// MQ-4: methane approximation.
const float MQ4_METHANE_A = 1012.7f;
const float MQ4_METHANE_B = -2.786f;

float rawToVoltage(int raw) {
  return ((float)raw * GAS_VCC) / GAS_ADC_MAX;
}

float rawToRsKOhm(int raw, float rlKOhm) {
  if (raw <= 0 || raw >= (int)GAS_ADC_MAX) {
    return NAN;
  }

  float voltage = rawToVoltage(raw);

  if (voltage <= 0.001f || voltage >= GAS_VCC) {
    return NAN;
  }

  return rlKOhm * ((GAS_VCC / voltage) - 1.0f);
}

float rsToPPM(
  float rsKOhm,
  float roKOhm,
  float curveA,
  float curveB
) {
  if (!isfinite(rsKOhm) ||
      !isfinite(roKOhm) ||
      rsKOhm <= 0.0f ||
      roKOhm <= 0.0f) {
    return NAN;
  }

  float ratio = rsKOhm / roKOhm;
  float ppm = curveA * pow(ratio, curveB);

  // Prevent invalid or absurd output from ADC saturation/noise.
  if (!isfinite(ppm) || ppm < 0.0f) {
    return NAN;
  }

  return ppm;
}

float rawToPPM_MQ2(int raw) {
  float rsKOhm = rawToRsKOhm(raw, MQ2_RL_KOHM);

  // MQ-2 result is LPG-equivalent, not gas-specific.
  return rsToPPM(
    rsKOhm,
    MQ2_RO_KOHM,
    MQ2_LPG_A,
    MQ2_LPG_B
  );
}

float rawToPPM_MQ4(int raw) {
  float rsKOhm = rawToRsKOhm(raw, MQ4_RL_KOHM);

  // MQ-4 result is an approximate methane estimate.
  return rsToPPM(
    rsKOhm,
    MQ4_RO_KOHM,
    MQ4_METHANE_A,
    MQ4_METHANE_B
  );
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Wire.begin();

  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("{\"error\":\"MLX90640 not found -- check wiring/address\"}");
    while (1) { delay(1000); }
  }
  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_4_HZ);

  SPI.begin();
  pinMode(ARDUCAM_CS_PIN, OUTPUT);
  myCAM.write_reg(0x07, 0x80);
  delay(100);
  myCAM.write_reg(0x07, 0x00);
  delay(100);
  myCAM.set_format(JPEG);
  myCAM.InitCAM();
  myCAM.OV2640_set_JPEG_size(OV2640_320x240);   // keep resolution low -- this is going over serial, not USB video

  analogReadResolution(12);  // UNO R4 supports up to 14-bit; 12-bit keeps the ppm math simple for now

  connectWiFi();
  runSelfTest();
  runWarmup();
}

void loop() {
  handleWebClient();

  unsigned long now = millis();

  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    sendTelemetry();
  }

  if (now - lastImage >= IMAGE_INTERVAL_MS) {
    lastImage = now;
    captureAndSendImage();
  }
}

// ---------------------------------------------------------------------------
// WiFi + local web server (for demo / direct dashboard polling)
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println();
      Serial.println("WiFi FAILED to connect -- check SSID/password. Continuing without WiFi.");
      return;
    }
  }
  Serial.println();

  // Sometimes status flips to CONNECTED a moment before DHCP actually
  // hands out an address -- retry reading it for a few seconds.
  IPAddress ip = WiFi.localIP();
  unsigned long ipStart = millis();
  while (ip == IPAddress(0, 0, 0, 0) && millis() - ipStart < 5000) {
    delay(300);
    ip = WiFi.localIP();
  }

  Serial.print("Arduino IP address: ");
  Serial.println(ip);
  Serial.print("WiFi status code: ");
  Serial.println(WiFi.status());   // 3 = WL_CONNECTED, useful if this ever misbehaves again
  server.begin();
}

void handleWebClient() {
  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("[web] client connected");

  unsigned long waitStart = millis();
  while (client.connected() && !client.available()) {
    if (millis() - waitStart > 1000) {
      Serial.println("[web] timed out waiting for request data");
      client.stop();
      return;
    }
  }

  String requestLine = client.readStringUntil('\r');   // e.g. "GET /image?t=123 HTTP/1.1"
  while (client.available()) {
    String headerLine = client.readStringUntil('\r');
    if (headerLine.length() <= 1) break;   // blank line = end of headers
  }
  Serial.print("[web] request: ");
  Serial.println(requestLine);

  if (requestLine.indexOf("/image") >= 0) {
    sendImageOverHttp(client);
  } else {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.print("Content-Length: ");
    client.println(latestJson.length());
    client.println("Connection: close");
    client.println();
    client.print(latestJson);
    client.flush();
  }

  delay(10);
  client.stop();
  Serial.println("[web] response sent, client closed");
}

// Triggers a fresh ArduCAM capture and streams the JPEG bytes directly to
// the HTTP client -- not cached in RAM (the board doesn't have room to spare
// for a second image buffer alongside WiFi + JSON + thermal frame data).
// This means requesting /image takes ~1-2s to respond and briefly blocks
// the main loop, same as the periodic Serial capture does.
void sendImageOverHttp(WiFiClient &client) {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  unsigned long waitStart = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - waitStart > 3000) {
      client.println("HTTP/1.1 504 Gateway Timeout");
      client.println("Connection: close");
      client.println();
      return;
    }
  }

  uint32_t len = myCAM.read_fifo_length();
  if (len == 0 || len > 500000) {
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println("Connection: close");
    client.println();
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: image/jpeg");
  client.println("Access-Control-Allow-Origin: *");
  client.print("Content-Length: ");
  client.println(len);
  client.println("Connection: close");
  client.println();

  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  for (uint32_t i = 0; i < len; i++) {
    client.write(SPI.transfer(0x00));
  }
  myCAM.CS_HIGH();
  client.flush();
}

// ---------------------------------------------------------------------------
// Self-test -- run once at boot, prints PASS/FAIL info to Serial
// ---------------------------------------------------------------------------
void runSelfTest() {
  // --- Thermal check ---
  if (mlx.getFrame(frame) == 0) {
    float minC = 999, maxC = -999, sumC = 0;
    for (int i = 0; i < 32 * 24; i++) {
      float t = frame[i];
      sumC += t;
      if (t < minC) minC = t;
      if (t > maxC) maxC = t;
    }
    float avgC = sumC / (32 * 24);
    Serial.print("Thermal min=");
    Serial.print(minC, 2);
    Serial.print(" C avg=");
    Serial.print(avgC, 2);
    Serial.print(" C max=");
    Serial.print(maxC, 2);
    Serial.println(" C");
  } else {
    Serial.println("Thermal FAIL -- no frame returned");
  }

  // --- Camera check -- do one real capture and report size ---
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  unsigned long waitStart = millis();
  bool capOk = true;
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - waitStart > 3000) { capOk = false; break; }
  }

  if (capOk) {
    uint32_t len = myCAM.read_fifo_length();
    if (len > 0 && len < 500000) {
      Serial.print("Camera PASS JPEG bytes=");
      Serial.println(len);
    } else {
      Serial.println("Camera FAIL -- bad FIFO length");
    }
  } else {
    Serial.println("Camera FAIL -- capture timed out");
  }
}

// ---------------------------------------------------------------------------
// Warm-up -- MQ sensor heaters need a few minutes before readings settle
// ---------------------------------------------------------------------------
void runWarmup() {
  Serial.println("Warming up gas sensors (3 min)...");
  unsigned long warmupStart = millis();
  unsigned long lastPrint = 0;

  while (millis() - warmupStart < WARMUP_MS) {
    handleWebClient();   // keep serving the web server during warmup too

    unsigned long elapsed = millis() - warmupStart;
    if (lastPrint == 0 || elapsed - lastPrint >= 10000) {
      lastPrint = elapsed;
      unsigned long remaining = (WARMUP_MS - elapsed) / 1000;
      Serial.print("Warmup: ");
      Serial.print(remaining);
      Serial.print("s remaining -- MQ2 raw=");
      Serial.print(analogRead(A0));
      Serial.print(" MQ4 raw=");
      Serial.println(analogRead(A1));
    }
  }
  Serial.println("Warmup complete. Starting telemetry.");
}

// ---------------------------------------------------------------------------
// Normal telemetry
// ---------------------------------------------------------------------------
void sendTelemetry() {
  seq++;

  // --- thermal frame ---
  bool ok = (mlx.getFrame(frame) == 0);
  float maxC = -999, sumC = 0;
  int hotspotCount = 0;
  int hotspotPx[10][2];   // cap reported hotspot pixels at 10 to keep the packet small
  int hotspotFound = 0;

  if (ok) {
    for (int i = 0; i < 32 * 24; i++) {
      float t = frame[i];
      sumC += t;
      if (t > maxC) maxC = t;
      if (t > HOTSPOT_THRESHOLD_C && hotspotFound < 10) {
        hotspotPx[hotspotFound][0] = i % 32;
        hotspotPx[hotspotFound][1] = i / 32;
        hotspotFound++;
        hotspotCount++;
      }
    }
  }
  float avgC = ok ? (sumC / (32 * 24)) : -999;

  // --- gas sensors ---
  // Average several readings to reduce ADC noise.
  const int GAS_SAMPLES = 10;

  long mq2Total = 0;
  long mq4Total = 0;

  for (int i = 0; i < GAS_SAMPLES; i++) {
    mq2Total += analogRead(A0);
    mq4Total += analogRead(A1);
    delay(2);
  }

  int rawMQ2 = mq2Total / GAS_SAMPLES;
  int rawMQ4 = mq4Total / GAS_SAMPLES;

  float mq2Ppm = rawToPPM_MQ2(rawMQ2);
  float mq4Ppm = rawToPPM_MQ4(rawMQ4);

  // --- build JSON (matches the schema pi_sensor_publisher.py / dashboard.html expect) ---
  StaticJsonDocument<512> doc;
  doc["seq"] = seq;
  doc["millis"] = millis();

  JsonObject thermal = doc.createNestedObject("thermal");
  thermal["max_c"] = round(maxC * 10) / 10.0;
  thermal["avg_c"] = round(avgC * 10) / 10.0;
  thermal["hotspot_count"] = hotspotCount;
  JsonArray hp = thermal.createNestedArray("hotspot_px");
  for (int i = 0; i < hotspotFound; i++) {
    JsonArray pair = hp.createNestedArray();
    pair.add(hotspotPx[i][0]);
    pair.add(hotspotPx[i][1]);
  }

  JsonObject gas = doc.createNestedObject("gas_ppm");

  // MQ-2 is cross-sensitive, so this is LPG-equivalent rather than
  // specifically smoke or methane.
  if (isfinite(mq2Ppm)) {
    gas["mq2_smoke"] = round(mq2Ppm);
  } else {
    gas["mq2_smoke"] = nullptr;
  }

  if (isfinite(mq4Ppm)) {
    gas["mq4_methane"] = round(mq4Ppm);
  } else {
    gas["mq4_methane"] = nullptr;
  }

  doc["image_available"] = false;   // images are sent as a separate frame -- see captureAndSendImage()

  serializeJson(doc, latestJson);   // cache for the web server
  serializeJson(doc, Serial);
  Serial.println();   // newline-delimited JSON: one line per reading, exactly what pi_sensor_publisher.py reads
}

void captureAndSendImage() {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  unsigned long waitStart = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - waitStart > 3000) {   // don't hang forever if the camera glitches
      Serial.println("{\"error\":\"image capture timed out\"}");
      return;
    }
  }

  uint32_t len = myCAM.read_fifo_length();
  if (len == 0 || len > 500000) {
    Serial.println("{\"error\":\"bad image length\"}");
    return;
  }

  // Header line: tells the Pi an image is about to stream and exactly how
  // many raw bytes to read before it hits the footer line.
  StaticJsonDocument<128> header;
  header["image_frame_start"] = true;
  header["size"] = len;
  serializeJson(header, Serial);
  Serial.println();

  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  for (uint32_t i = 0; i < len; i++) {
    Serial.write(SPI.transfer(0x00));
  }
  myCAM.CS_HIGH();

  Serial.println();
  Serial.println("{\"image_frame_end\":true}");
}
/*
  scout_sensor_hub.ino
  Runs on the Arduino UNO R4 WiFi.

  Reads the MLX90640 thermal camera + 4 MQ gas sensors (analog), packages
  each reading as one line of JSON over USB serial -- matching the schema
  pi_sensor_publisher.py expects. Periodically triggers an Arducam still
  capture and streams the raw JPEG bytes over the same serial link, wrapped
  in header/footer JSON markers so the Pi can tell where the image starts
  and ends.

  Libraries needed (Arduino IDE -> Tools -> Manage Libraries):
    - Adafruit MLX90640
    - ArduinoJson
    - ArduCAM (install per ArduCAM's own GitHub instructions -- their library
      isn't in the standard Library Manager index)

  NOTE: this is a starting sketch, not a finished one. A few things are
  placeholders you'll need to tune on the bench -- flagged with comments
  below. Exact API calls for Adafruit_MLX90640 and ArduCAM can vary a bit
  by library version, so double check function names against whatever
  version you actually install.

  Wiring: matches the wiring chart from earlier -- shared I2C bus (SDA/SCL)
  for MLX90640 + Arducam, SPI pins 10-13 to Arducam, A0-A3 to the 4 gas
  sensors (MQ-2, MQ-4, MQ-7, MQ-135).
*/

#include <Wire.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Adafruit_MLX90640.h>
#include <ArduCAM.h>

// ---------------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------------
#define ARDUCAM_CS_PIN 10
const unsigned long TELEMETRY_INTERVAL_MS = 1000;    // send sensor JSON once per second
const unsigned long IMAGE_INTERVAL_MS     = 15000;   // capture + send a still every 15s
const float HOTSPOT_THRESHOLD_C = 38.0;              // PLACEHOLDER -- tune once you know ambient baseline at the dump

Adafruit_MLX90640 mlx;
ArduCAM myCAM(OV2640, ARDUCAM_CS_PIN);

float frame[32 * 24];       // raw thermal frame buffer (32x24 pixels)
unsigned long lastTelemetry = 0;
unsigned long lastImage = 0;
unsigned long seq = 0;

// ---------------------------------------------------------------------------
// Gas sensor calibration -- PLACEHOLDER SCALING
// These straight-line conversions are NOT real calibration curves. MQ
// sensors need to be calibrated against known gas concentrations (or at
// minimum a clean-air baseline reading) before these numbers mean anything
// as true ppm. Treat these as relative signal strength for now, and swap
// in real curves once you've done bench calibration -- see the sensor's
// datasheet for its Rs/Ro-vs-ppm curve.
// ---------------------------------------------------------------------------
float rawToPPM_MQ2(int raw)   { return raw * 0.35; }   // smoke/LPG
float rawToPPM_MQ4(int raw)   { return raw * 0.12; }   // methane
float rawToPPM_MQ7(int raw)   { return raw * 0.08; }   // carbon monoxide
float rawToPPM_MQ135(int raw) { return raw * 0.20; }   // air quality / VOC

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Wire.begin();

  if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("{\"error\":\"MLX90640 not found -- check wiring/address\"}");
    while (1) { delay(1000); }
  }
  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_4_HZ);

  SPI.begin();
  pinMode(ARDUCAM_CS_PIN, OUTPUT);
  myCAM.write_reg(0x07, 0x80);
  delay(100);
  myCAM.write_reg(0x07, 0x00);
  delay(100);
  myCAM.set_format(JPEG);
  myCAM.InitCAM();
  myCAM.OV2640_set_JPEG_size(OV2640_320x240);   // keep resolution low -- this is going over serial, not USB video

  analogReadResolution(12);  // UNO R4 supports up to 14-bit; 12-bit keeps the ppm math simple for now
}

void loop() {
  unsigned long now = millis();

  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    sendTelemetry();
  }

  if (now - lastImage >= IMAGE_INTERVAL_MS) {
    lastImage = now;
    captureAndSendImage();
  }
}

void sendTelemetry() {
  seq++;

  // --- thermal frame ---
  bool ok = (mlx.getFrame(frame) == 0);
  float maxC = -999, sumC = 0;
  int hotspotCount = 0;
  int hotspotPx[10][2];   // cap reported hotspot pixels at 10 to keep the packet small
  int hotspotFound = 0;

  if (ok) {
    for (int i = 0; i < 32 * 24; i++) {
      float t = frame[i];
      sumC += t;
      if (t > maxC) maxC = t;
      if (t > HOTSPOT_THRESHOLD_C && hotspotFound < 10) {
        hotspotPx[hotspotFound][0] = i % 32;
        hotspotPx[hotspotFound][1] = i / 32;
        hotspotFound++;
        hotspotCount++;
      }
    }
  }
  float avgC = ok ? (sumC / (32 * 24)) : -999;

  // --- gas sensors ---
  int rawMQ2   = analogRead(A0);
  int rawMQ4   = analogRead(A1);
  int rawMQ7   = analogRead(A2);
  int rawMQ135 = analogRead(A3);

  // --- build JSON (matches the schema pi_sensor_publisher.py / dashboard.html expect) ---
  StaticJsonDocument<512> doc;
  doc["seq"] = seq;
  doc["millis"] = millis();

  JsonObject thermal = doc.createNestedObject("thermal");
  thermal["max_c"] = round(maxC * 10) / 10.0;
  thermal["avg_c"] = round(avgC * 10) / 10.0;
  thermal["hotspot_count"] = hotspotCount;
  JsonArray hp = thermal.createNestedArray("hotspot_px");
  for (int i = 0; i < hotspotFound; i++) {
    JsonArray pair = hp.createNestedArray();
    pair.add(hotspotPx[i][0]);
    pair.add(hotspotPx[i][1]);
  }

  JsonObject gas = doc.createNestedObject("gas_ppm");
  gas["mq2_smoke"]   = round(rawToPPM_MQ2(rawMQ2));
  gas["mq4_methane"] = round(rawToPPM_MQ4(rawMQ4));
  gas["mq7_co"]      = round(rawToPPM_MQ7(rawMQ7));
  gas["mq135_voc"]   = round(rawToPPM_MQ135(rawMQ135));

  doc["image_available"] = false;   // images are sent as a separate frame -- see captureAndSendImage()

  serializeJson(doc, Serial);
  Serial.println();   // newline-delimited JSON: one line per reading, exactly what pi_sensor_publisher.py reads
}

void captureAndSendImage() {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  unsigned long waitStart = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - waitStart > 3000) {   // don't hang forever if the camera glitches
      Serial.println("{\"error\":\"image capture timed out\"}");
      return;
    }
  }

  uint32_t len = myCAM.read_fifo_length();
  if (len == 0 || len > 500000) {
    Serial.println("{\"error\":\"bad image length\"}");
    return;
  }

  // Header line: tells the Pi an image is about to stream and exactly how
  // many raw bytes to read before it hits the footer line.
  StaticJsonDocument<128> header;
  header["image_frame_start"] = true;
  header["size"] = len;
  serializeJson(header, Serial);
  Serial.println();

  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  for (uint32_t i = 0; i < len; i++) {
    Serial.write(SPI.transfer(0x00));
  }
  myCAM.CS_HIGH();

  Serial.println();
  Serial.println("{\"image_frame_end\":true}");
}
