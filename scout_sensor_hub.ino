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
