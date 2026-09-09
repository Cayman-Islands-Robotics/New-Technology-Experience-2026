#!/usr/bin/env python3
"""
gps_uplink.py

Phase-1 replacement for app.py's nav loop on the Orange Pi 5. The hub's
Autonomous OpMode now owns the waypoints (predetermined, set in code on
the hub side), so the Pi doesn't compute or send a route anymore -- its
only job is to keep the hub supplied with a fresh GPS position so it
shows up in Driver Station telemetry.

No Flask, no Leaflet UI, no mission queue. Reuses gnss_reader.py and
hub_link.py unchanged -- neither of them cares what board it runs on.

Talks to the hub over USB (via `adb forward`) rather than WiFi -- this
board has no WiFi chip. Run start_gps_uplink.sh, not this file directly,
unless the adb forwards are already set up in your current shell.

Run:
    python3 gps_uplink.py                                  # defaults
    python3 gps_uplink.py --port /dev/ttyUSB3 --hub-ip 127.0.0.1

Note on the wire format: HubLink.set_target() still takes target_lat/
target_lon because that's what the hub's TCP parser currently expects
in a "target" message (see hub_link.py's docstring). Since the hub is
doing its own navigation now, those fields are dead weight from the
Pi's point of view -- this script just echoes cur_lat/cur_lon back into
them as harmless placeholders rather than sending nulls. If/when
NavWebControl's parser is updated to accept a position-only message
type, this is the spot to change.
"""

import argparse
import time

from gnss_reader import GNSSReader
from hub_link import HubLink

SEND_HZ = 5.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/ttyUSB2",
                        help="SIM7600X AT command port.")
    parser.add_argument("--hub-ip", default="127.0.0.1",
                        help="127.0.0.1 when tunneled over adb forward (USB link, no WiFi).")
    parser.add_argument("--hub-port", type=int, default=6000)
    args = parser.parse_args()

    gnss = GNSSReader(port=args.port)
    hub = HubLink(hub_ip=args.hub_ip, hub_port=args.hub_port)

    gnss.start()
    hub.start()

    interval = 1.0 / SEND_HZ
    print(f"[gps_uplink] streaming fixes to {args.hub_ip}:{args.hub_port}")

    try:
        while True:
            fix = gnss.get_fix()
            if fix is None:
                # No fresh fix: stop feeding the hub rather than resend a
                # stale position. Same reasoning as app.py's nav loop --
                # let the hub's failsafe/telemetry reflect reality.
                hub.clear_target()
                print("[gps_uplink] no fresh fix")
            else:
                hub.set_target(
                    cur_lat=fix["lat"],
                    cur_lon=fix["lon"],
                    target_lat=fix["lat"],
                    target_lon=fix["lon"],
                    fix_id=fix.get("fix_id", 0),
                    wp_id=0,
                    hdop=fix.get("hdop"),
                    num_sats=fix.get("num_sats"),
                )
                print(f"[gps_uplink] {fix['lat']:.6f}, {fix['lon']:.6f} "
                      f"sats={fix.get('num_sats')} hdop={fix.get('hdop')}")
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n[gps_uplink] stopping")
    finally:
        hub.stop()
        gnss.stop()


if __name__ == "__main__":
    main()
