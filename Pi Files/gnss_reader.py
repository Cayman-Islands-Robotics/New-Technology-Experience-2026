"""
gnss_reader.py

Reads position from a Waveshare SIM7600X's built-in GNSS. The SIM7600X
does NOT stream continuous NMEA on its own -- you talk to it over its AT
command port and poll for a fix.

Changes from the first draft:
  - Polls AT+CGNSSINFO (not AT+CGPSINFO) so we get fix mode, per-
    constellation satellite counts and HDOP/PDOP/VDOP. The UI showed
    "sats: undefined" before because CGPSINFO never returns a sat count.
    Falls back to CGPSINFO automatically if the firmware rejects it.
  - Reads until OK/ERROR instead of sleeping a fixed 0.5s and grabbing
    whatever happened to be in the buffer.
  - get_fix() refuses to return a fix older than max_age_s. Stale fixes
    were the worst bug in the old version: with no fix, the Pi would keep
    resending the last known position forever, the hub would keep
    re-anchoring dead reckoning to it, and the rover would drive without
    its position estimate ever changing -- never arriving, never stopping.
  - Every fix carries a monotonic fix_id so the hub can tell a genuinely
    new fix from a repeat of one it has already anchored on.
  - Serial access is locked, and the port is closed/reopened on error
    instead of spinning on a dead handle.
  - Speed is converted from knots (what the modem reports) to km/h.

Requires: pyserial
    pip install pyserial --break-system-packages

SIM7600X setup notes:
  - `ls /dev/ttyUSB*` will show several ports. One is the AT command
    port -- commonly ttyUSB2 or ttyUSB3 on Waveshare's Pi images, but it
    varies. Confirm with `sudo minicom -D /dev/ttyUSB2 -b 115200`, type
    AT + Enter, look for OK.
  - GNSS is started with AT+CGPS=1 (done automatically in start()).
    First fix outdoors from cold can take 30s to several minutes.
  - The antenna needs a clear view of sky. No fix indoors.
  - Coordinates come back as ddmm.mmmmmm (degrees + decimal minutes),
    not decimal degrees. Converted here.
"""

import threading
import time

import serial

KNOTS_TO_KMH = 1.852


def _ddmm_to_decimal(value):
    """Converts ddmm.mmmmmm (or dddmm.mmmmmm for longitude) to decimal degrees."""
    if not value or "." not in value:
        return None
    try:
        dot = value.index(".")
        minutes_start = dot - 2
        if minutes_start <= 0:
            return None
        degrees = int(value[:minutes_start])
        minutes = float(value[minutes_start:])
        return degrees + minutes / 60.0
    except (ValueError, IndexError):
        return None


def _f(value):
    """float() that tolerates empty fields."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _i(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


class GNSSReader:
    # How old a fix may be before get_fix() reports "no fix". At 1Hz
    # polling, anything over a few seconds means the modem has lost lock.
    DEFAULT_MAX_AGE_S = 5.0

    def __init__(self, port="/dev/ttyUSB2", baud=115200, poll_hz=1.0):
        self.port = port
        self.baud = baud
        self.poll_interval = 1.0 / poll_hz

        self._lock = threading.Lock()          # guards _fix / counters
        self._serial_lock = threading.Lock()   # guards the serial port
        self._fix = None
        self._fix_id = 0
        self._running = False
        self._thread = None
        self._ser = None
        self._use_cgnssinfo = True
        self._consecutive_errors = 0

    # ---- lifecycle -------------------------------------------------

    def start(self):
        self._open_serial()
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)
        self._close_serial()

    # ---- public state ----------------------------------------------

    def get_fix(self, max_age_s=DEFAULT_MAX_AGE_S):
        """Most recent fix as a dict, or None if there isn't a fresh one.

        Returning None is the correct, safe answer when the modem has lost
        lock: the caller stops sending targets, the hub's failsafe trips,
        and the rover brakes.
        """
        with self._lock:
            if not self._fix:
                return None
            if max_age_s is not None and time.time() - self._fix["timestamp"] > max_age_s:
                return None
            return dict(self._fix)

    def get_last_fix(self):
        """Last fix regardless of age. For display/debug only -- never feed
        this to the navigation loop."""
        with self._lock:
            return dict(self._fix) if self._fix else None

    def is_healthy(self):
        return self._consecutive_errors < 5

    # ---- serial plumbing -------------------------------------------

    def _open_serial(self):
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
        time.sleep(0.2)
        self._ser.reset_input_buffer()
        self._enable_gps()

    def _close_serial(self):
        try:
            if self._ser:
                self._ser.close()
        except Exception:
            pass
        self._ser = None

    def _reopen_serial(self):
        print("[GNSS] reopening serial port")
        self._close_serial()
        time.sleep(1.0)
        self._open_serial()

    def _send_at(self, command, timeout=2.0):
        """Writes an AT command and reads until OK/ERROR or timeout."""
        with self._serial_lock:
            self._ser.reset_input_buffer()
            self._ser.write((command + "\r\n").encode())
            deadline = time.time() + timeout
            buf = ""
            while time.time() < deadline:
                chunk = self._ser.read(256)
                if chunk:
                    buf += chunk.decode(errors="replace")
                    if "OK" in buf or "ERROR" in buf:
                        break
                else:
                    time.sleep(0.02)
            return buf

    def _enable_gps(self):
        resp = self._send_at("AT+CGPS=1", timeout=3.0)
        # An error here usually just means GNSS was already on from a
        # previous run, which is fine.
        print(f"[GNSS] AT+CGPS=1 -> {resp.strip()!r}")

    # ---- parsing ---------------------------------------------------

    @staticmethod
    def _parse_cgnssinfo(resp):
        """+CGNSSINFO: <mode>,<GPS-SVs>,<GLONASS-SVs>,<BEIDOU-SVs>,<lat>,<N/S>,
        <lon>,<E/W>,<date>,<utc>,<alt>,<speed>,<course>,<PDOP>,<HDOP>,<VDOP>"""
        for line in resp.splitlines():
            line = line.strip()
            if not line.startswith("+CGNSSINFO:"):
                continue
            fields = [f.strip() for f in line.split(":", 1)[1].split(",")]
            if len(fields) < 16 or not fields[4] or not fields[6]:
                return None  # no fix yet
            lat = _ddmm_to_decimal(fields[4])
            lon = _ddmm_to_decimal(fields[6])
            if lat is None or lon is None:
                return None
            if fields[5] == "S":
                lat = -lat
            if fields[7] == "W":
                lon = -lon
            speed_knots = _f(fields[11])
            return {
                "lat": lat,
                "lon": lon,
                "fix_mode": _i(fields[0]),           # 2 = 2D, 3 = 3D
                "num_sats": _i(fields[1]) + _i(fields[2]) + _i(fields[3]),
                "altitude_m": _f(fields[10]),
                "speed_kmh": speed_knots * KNOTS_TO_KMH if speed_knots is not None else None,
                "course_deg": _f(fields[12]),
                "pdop": _f(fields[13]),
                "hdop": _f(fields[14]),
                "vdop": _f(fields[15]),
            }
        return None

    @staticmethod
    def _parse_cgpsinfo(resp):
        """Fallback: +CGPSINFO: <lat>,<N/S>,<lon>,<E/W>,<date>,<utc>,<alt>,
        <speed>,<course>. No satellite count or HDOP available."""
        for line in resp.splitlines():
            line = line.strip()
            if not line.startswith("+CGPSINFO:"):
                continue
            fields = [f.strip() for f in line.split(":", 1)[1].split(",")]
            if len(fields) < 9 or not fields[0] or not fields[2]:
                return None
            lat = _ddmm_to_decimal(fields[0])
            lon = _ddmm_to_decimal(fields[2])
            if lat is None or lon is None:
                return None
            if fields[1] == "S":
                lat = -lat
            if fields[3] == "W":
                lon = -lon
            speed_knots = _f(fields[7])
            return {
                "lat": lat,
                "lon": lon,
                "fix_mode": 0,
                "num_sats": 0,
                "altitude_m": _f(fields[6]),
                "speed_kmh": speed_knots * KNOTS_TO_KMH if speed_knots is not None else None,
                "course_deg": _f(fields[8]),
                "pdop": None,
                "hdop": None,
                "vdop": None,
            }
        return None

    # ---- poll loop -------------------------------------------------

    def _run(self):
        while self._running:
            try:
                if self._use_cgnssinfo:
                    resp = self._send_at("AT+CGNSSINFO")
                    if "ERROR" in resp:
                        print("[GNSS] CGNSSINFO not supported, falling back to CGPSINFO")
                        self._use_cgnssinfo = False
                        continue
                    parsed = self._parse_cgnssinfo(resp)
                else:
                    resp = self._send_at("AT+CGPSINFO")
                    parsed = self._parse_cgpsinfo(resp)

                self._consecutive_errors = 0

                if parsed:
                    with self._lock:
                        self._fix_id += 1
                        parsed["fix_id"] = self._fix_id
                        parsed["timestamp"] = time.time()
                        self._fix = parsed
                # else: empty fields, no lock yet. Deliberately do NOT
                # refresh the timestamp -- the fix ages out and get_fix()
                # starts returning None.

            except Exception as e:
                self._consecutive_errors += 1
                print(f"[GNSS] error ({self._consecutive_errors}): {e}")
                if self._consecutive_errors >= 3:
                    try:
                        self._reopen_serial()
                        self._consecutive_errors = 0
                    except Exception as reopen_error:
                        print(f"[GNSS] reopen failed: {reopen_error}")
                        time.sleep(2.0)

            time.sleep(self.poll_interval)


if __name__ == "__main__":
    # Standalone test: python3 gnss_reader.py
    reader = GNSSReader(port="/dev/ttyUSB2", baud=115200)
    reader.start()
    print("Polling SIM7600X GNSS. Ctrl+C to stop.")
    print("Outdoors with clear sky, first fix may take a few minutes.")
    print("Log this for 10 minutes while stationary -- the scatter you see")
    print("is what your arrival radius has to be larger than.")
    try:
        while True:
            fix = reader.get_fix()
            if fix:
                print(f"{fix['lat']:.6f}, {fix['lon']:.6f}  "
                      f"sats={fix['num_sats']} hdop={fix['hdop']} mode={fix['fix_mode']}")
            else:
                print("no fresh fix")
            time.sleep(2)
    except KeyboardInterrupt:
        reader.stop()
