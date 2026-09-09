"""
hub_link.py

Connects to the Control Hub's TCP listener (port 6000) as a client.
Sends newline-delimited JSON "target" messages and reads back
newline-delimited JSON "status" messages.

Changes from the first draft:
  - Reconnects automatically with backoff. Before, a single dropped
    socket set _running = False and the Pi was dead until you restarted
    the process -- on a moving robot over WiFi that is a matter of when,
    not if.
  - TCP_NODELAY so Nagle doesn't batch these small messages and add
    latency to the control loop, and SO_KEEPALIVE so a WiFi dropout
    surfaces as an error instead of blocking forever in sendall().
  - send_estop() goes out immediately rather than waiting for the next
    heartbeat tick.
  - Targets carry fix_id (so the hub can tell a new GNSS fix from a
    repeat), wp_id (so the Pi only advances the mission when the hub
    confirms arrival at the waypoint it is actually on), hdop, and
    num_sats.

Protocol
  Pi -> Hub, at HEARTBEAT_HZ:
    {"type":"target","cur_lat":..,"cur_lon":..,"target_lat":..,
     "target_lon":..,"fix_id":<int>,"wp_id":<int>,"hdop":<float|null>,
     "num_sats":<int|null>,"seq":<int>,"ts":<unix>}
  Pi -> Hub, on demand:
    {"type":"estop"}  /  {"type":"clear_estop"}
  Hub -> Pi, every control tick:
    {"type":"status","state":"driving|arrived|stopped|calibrating|fault",
     "dr_lat":..,"dr_lon":..,"heading_deg":..,"heading_calibrated":bool,
     "wp_id":<int>,"distance_m":..,"estop":bool,"ts":<unix>}
"""

import collections
import itertools
import json
import socket
import threading
import time


class HubLink:
    def __init__(self, hub_ip="192.168.43.1", hub_port=6000, heartbeat_hz=5.0):
        self.hub_ip = hub_ip
        self.hub_port = hub_port
        self.heartbeat_interval = 1.0 / heartbeat_hz

        self._sock = None
        self._sock_lock = threading.Lock()   # guards writes to the socket
        self._state_lock = threading.Lock()  # guards target/status/flags
        self._running = False

        self._latest_target = None
        self._latest_status = None
        self._status_ts = 0.0
        self._connected = False
        self._seq = 0

        self._supervisor = None
        self._recv_thread = None

        # Ring buffer of recent traffic, for the dashboard's link log.
        # Bounded so a long mission can't grow it without limit.
        self._log = collections.deque(maxlen=300)
        self._log_ids = itertools.count(1)

    # ---- lifecycle -------------------------------------------------

    def start(self):
        """Non-blocking. Spawns a supervisor that keeps the link up."""
        self._running = True
        self._supervisor = threading.Thread(target=self._supervise, daemon=True)
        self._supervisor.start()

    # Kept for compatibility with the old API.
    connect = start

    def stop(self):
        self._running = False
        self._close_socket()
        if self._supervisor:
            self._supervisor.join(timeout=3)

    disconnect = stop

    def is_connected(self):
        with self._state_lock:
            return self._connected

    # ---- public state ----------------------------------------------

    def set_target(self, cur_lat, cur_lon, target_lat, target_lon,
                   fix_id=0, wp_id=0, hdop=None, num_sats=None):
        with self._state_lock:
            self._latest_target = {
                "cur_lat": cur_lat,
                "cur_lon": cur_lon,
                "target_lat": target_lat,
                "target_lon": target_lon,
                "fix_id": fix_id,
                "wp_id": wp_id,
                "hdop": hdop,
                "num_sats": num_sats,
            }

    def clear_target(self):
        """Stop sending targets. The hub's failsafe will brake the rover
        after FAILSAFE_TIMEOUT_MS. Use send_estop() if you want it to stop
        right now."""
        with self._state_lock:
            self._latest_target = None

    def get_status(self):
        """Latest status from the hub, or None if it is stale/absent."""
        with self._state_lock:
            if not self._latest_status:
                return None
            if time.time() - self._status_ts > 3.0:
                return None
            return dict(self._latest_status)

    def send_manual(self, left, right):
        """Direct motor power command. Bypasses waypoint navigation."""
        return self._send_now({"type": "manual", "left": left,
                               "right": right, "ts": time.time()})

    def send_manual_off(self):
        return self._send_now({"type": "manual_off", "ts": time.time()})

    def send_estop(self):
        return self._send_now({"type": "estop", "ts": time.time()})

    def clear_estop(self):
        return self._send_now({"type": "clear_estop", "ts": time.time()})

    def get_log(self, since=0, limit=200):
        """Traffic newer than `since` (an entry id), oldest first."""
        with self._state_lock:
            entries = [e for e in self._log if e["id"] > since]
        return entries[-limit:]

    def _log_entry(self, direction, text):
        entry = {
            "id": next(self._log_ids),
            "dir": direction,          # tx | rx | sys
            "t": time.time(),
            "text": text[:400],
        }
        with self._state_lock:
            self._log.append(entry)

    # ---- internals -------------------------------------------------

    def _send_now(self, msg):
        payload = (json.dumps(msg) + "\n").encode("utf-8")
        with self._sock_lock:
            if not self._sock:
                return False
            try:
                self._sock.sendall(payload)
                self._log_entry("tx", json.dumps(msg))
                return True
            except OSError as e:
                print(f"[HubLink] send failed: {e}")
                self._log_entry("sys", f"send failed: {e}")
                return False

    def _open_socket(self):
        sock = socket.create_connection((self.hub_ip, self.hub_port), timeout=5)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        sock.settimeout(None)
        return sock

    def _close_socket(self):
        with self._sock_lock:
            try:
                if self._sock:
                    self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                if self._sock:
                    self._sock.close()
            except OSError:
                pass
            self._sock = None
        with self._state_lock:
            self._connected = False

    def _supervise(self):
        backoff = 1.0
        while self._running:
            try:
                sock = self._open_socket()
                with self._sock_lock:
                    self._sock = sock
                with self._state_lock:
                    self._connected = True
                print(f"[HubLink] connected to {self.hub_ip}:{self.hub_port}")
                self._log_entry("sys", f"connected to {self.hub_ip}:{self.hub_port}")
                backoff = 1.0

                self._recv_thread = threading.Thread(target=self._recv_loop,
                                                     args=(sock,), daemon=True)
                self._recv_thread.start()

                self._send_loop(sock)  # returns when the link drops

            except (OSError, socket.timeout) as e:
                print(f"[HubLink] connect failed: {e}")
                self._log_entry("sys", f"connect failed: {e}")

            self._close_socket()
            if not self._running:
                break
            time.sleep(backoff)
            backoff = min(backoff * 2, 5.0)

    def _send_loop(self, sock):
        while self._running:
            with self._state_lock:
                target = dict(self._latest_target) if self._latest_target else None
                self._seq += 1
                seq = self._seq

            if target:
                msg = {"type": "target", "seq": seq, "ts": time.time(), **target}
                try:
                    with self._sock_lock:
                        if self._sock is not sock:
                            return
                        sock.sendall((json.dumps(msg) + "\n").encode("utf-8"))
                    self._log_entry("tx", json.dumps(msg))
                except OSError as e:
                    print(f"[HubLink] send failed, will reconnect: {e}")
                    return

            time.sleep(self.heartbeat_interval)

    def _recv_loop(self, sock):
        buf = b""
        while self._running:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        msg = json.loads(line.decode("utf-8"))
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        continue  # one bad line shouldn't kill the link
                    self._log_entry("rx", line.decode("utf-8", "replace").strip())
                    if msg.get("type") == "status":
                        with self._state_lock:
                            self._latest_status = msg
                            self._status_ts = time.time()
            except OSError as e:
                print(f"[HubLink] recv error: {e}")
                break
        print("[HubLink] receive loop ended")


if __name__ == "__main__":
    link = HubLink(hub_ip="192.168.43.1", hub_port=6000)
    link.start()
    link.set_target(cur_lat=37.4220, cur_lon=-122.0841,
                    target_lat=37.4225, target_lon=-122.0845,
                    fix_id=1, wp_id=0)
    try:
        while True:
            print("connected:", link.is_connected(), "status:", link.get_status())
            time.sleep(1)
    except KeyboardInterrupt:
        link.stop()
