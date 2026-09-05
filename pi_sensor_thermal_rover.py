"""
pi_sensor_publisher.py

Runs on the Raspberry Pi. Reads sensor JSON lines coming from the Arduino
over USB serial, reads a GPS fix from the SIM7600G-H HAT, stamps each
reading with location + time, and pushes it to Firebase (Firestore for
the data, Storage for any camera images) so the dashboard app can pick
it up live over the cellular connection -- no webserver needed on the Pi.

Project: thermal-rover

Setup (one-time):
    pip install pyserial firebase-admin python-dotenv --break-system-packages
    Download a service account key for the THERMAL-ROVER project from:
      Firebase console -> Project settings -> Service accounts -> Generate new private key
    Copy its values into a .env file next to this script (see .env.example):
      FIREBASE_PRIVATE_KEY, FIREBASE_CLIENT_EMAIL, etc.
    (If you still have a key from the old scout-01-dashboard project lying
    around on the Pi, delete/replace it -- a key only works for the project
    it was generated from.)

Run:
    python3 pi_sensor_publisher.py
"""

import json
import os
import time
import serial
import firebase_admin
from firebase_admin import credentials, firestore, storage
from dotenv import load_dotenv

load_dotenv()

# ---------------------------------------------------------------------------
# CONFIG -- fill these in for your project
# ---------------------------------------------------------------------------
ARDUINO_PORT = os.getenv("ARDUINO_PORT", "/dev/ttyACM0")
ARDUINO_BAUD = int(os.getenv("ARDUINO_BAUD", "115200"))

GPS_PORT = os.getenv("GPS_PORT", "/dev/ttyUSB1")
GPS_BAUD = int(os.getenv("GPS_BAUD", "115200"))

FIREBASE_STORAGE_BUCKET = os.getenv("FIREBASE_STORAGE_BUCKET", "thermal-rover.firebasestorage.app")

READINGS_COLLECTION = os.getenv("READINGS_COLLECTION", "readings")

# ---------------------------------------------------------------------------
# Firebase setup
# ---------------------------------------------------------------------------
firebase_cred = credentials.Certificate({
    "type": os.getenv("FIREBASE_TYPE"),
    "project_id": os.getenv("FIREBASE_PROJECT_ID"),
    "private_key_id": os.getenv("FIREBASE_PRIVATE_KEY_ID"),
    "private_key": os.getenv("FIREBASE_PRIVATE_KEY"),
    "client_email": os.getenv("FIREBASE_CLIENT_EMAIL"),
    "client_id": os.getenv("FIREBASE_CLIENT_ID"),
    "auth_uri": os.getenv("FIREBASE_AUTH_URI"),
    "token_uri": os.getenv("FIREBASE_TOKEN_URI"),
    "auth_provider_x509_cert_url": os.getenv("FIREBASE_AUTH_PROVIDER_X509_CERT_URL"),
    "client_x509_cert_url": os.getenv("FIREBASE_CLIENT_X509_CERT_URL"),
})
firebase_admin.initialize_app(firebase_cred, {"storageBucket": FIREBASE_STORAGE_BUCKET})
db = firestore.client()
bucket = storage.bucket()


def get_gps_fix(gps_serial):
    """
    Very small NMEA parser for a GPGGA sentence to pull lat/long out of the
    SIM7600's GNSS output. Returns (lat, lon) or (None, None) if no fix yet.
    In practice you may prefer the `pynmea2` library for a more robust parse --
    this is intentionally minimal so it's easy to read/debug on the bench.
    """
    line = gps_serial.readline().decode(errors="ignore").strip()
    if line.startswith("$GPGGA") or line.startswith("$GNGGA"):
        parts = line.split(",")
        if len(parts) > 5 and parts[2] and parts[4]:
            lat_raw, lat_dir = parts[2], parts[3]
            lon_raw, lon_dir = parts[4], parts[5]
            lat = float(lat_raw[:2]) + float(lat_raw[2:]) / 60
            if lat_dir == "S":
                lat = -lat
            lon = float(lon_raw[:3]) + float(lon_raw[3:]) / 60
            if lon_dir == "W":
                lon = -lon
            return lat, lon
    return None, None


def read_image_bytes(arduino_serial, size, timeout_s=5):
    """
    Reads exactly `size` raw bytes from the serial port following an
    image_frame_start header line. Uses .read(n) rather than .readline()
    for this part specifically, since raw JPEG data can contain byte
    values that look like newlines -- reading by count instead of by line
    avoids accidentally splitting the image data in the wrong place.

    After the raw bytes, the Arduino sends a blank line and then the
    image_frame_end footer line -- this consumes those before returning.
    """
    data = bytearray()
    start = time.time()
    while len(data) < size:
        chunk = arduino_serial.read(size - len(data))
        if chunk:
            data.extend(chunk)
        if time.time() - start > timeout_s:
            print(f"Timed out waiting for image bytes ({len(data)}/{size} received)")
            return None

    # Consume the trailing blank line + the image_frame_end footer line
    for _ in range(3):
        trailer = arduino_serial.readline().decode(errors="ignore").strip()
        if "image_frame_end" in trailer:
            break

    return bytes(data)


def save_image_bytes(image_bytes):
    """Saves captured JPEG bytes to a local file on the Pi, returns the path."""
    os.makedirs("/home/pi/captures", exist_ok=True)
    filename = f"/home/pi/captures/img_{int(time.time())}.jpg"
    with open(filename, "wb") as f:
        f.write(image_bytes)
    return filename



def upload_image(local_path, remote_name):
    """Uploads a captured still to Firebase Storage, returns a public URL."""
    blob = bucket.blob(f"images/{remote_name}")
    blob.upload_from_filename(local_path)
    blob.make_public()
    return blob.public_url


def publish_reading(reading, lat, lon, image_url=None):
    """Writes one sensor reading to Firestore. The dashboard listens to this
    collection in real time, so this is the only 'send' step needed."""
    doc = {
        **reading,
        "lat": lat,
        "lon": lon,
        "image_url": image_url,
        "server_time": firestore.SERVER_TIMESTAMP,
    }
    db.collection(READINGS_COLLECTION).add(doc)
    print(f"Published reading seq={reading.get('seq')} at ({lat}, {lon})")


def main():
    arduino = serial.Serial(ARDUINO_PORT, ARDUINO_BAUD, timeout=2)
    gps = serial.Serial(GPS_PORT, GPS_BAUD, timeout=1)

    last_lat, last_lon = None, None
    latest_image_url = None   # persists across readings until a newer image arrives

    while True:
        # 1. Update GPS fix if a new sentence is available
        lat, lon = get_gps_fix(gps)
        if lat is not None:
            last_lat, last_lon = lat, lon

        # 2. Read one line from the Arduino -- could be a sensor reading,
        #    an image frame header, or a stray footer line
        line = arduino.readline().decode(errors="ignore").strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print(f"Skipping malformed line: {line}")
            continue

        # 3a. This line announces an incoming image -- switch to reading
        #     raw bytes instead of another JSON line
        if msg.get("image_frame_start"):
            size = msg.get("size", 0)
            print(f"Receiving image frame ({size} bytes)...")
            image_bytes = read_image_bytes(arduino, size)
            if image_bytes:
                local_path = save_image_bytes(image_bytes)
                try:
                    latest_image_url = upload_image(local_path, os.path.basename(local_path))
                    print(f"Image uploaded: {latest_image_url}")
                except Exception as e:
                    print(f"Image upload failed: {e}")
            continue  # not a sensor reading -- nothing more to do this loop

        # 3b. A stray footer line arriving on its own (harmless, just skip)
        if msg.get("image_frame_end"):
            continue

        # 3c. A normal sensor reading -- attach whatever the latest known
        #     image is (may be from an earlier cycle, since images are sent
        #     on their own slower interval, not every reading)
        publish_reading(msg, last_lat, last_lon, latest_image_url)

        time.sleep(0.1)  # small breather; tune to your Arduino's send rate


if __name__ == "__main__":
    main()