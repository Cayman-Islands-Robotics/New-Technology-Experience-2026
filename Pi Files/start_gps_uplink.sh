#!/usr/bin/env bash
#
# start_gps_uplink.sh
#
# The hub link is over USB now (no WiFi chip on this board), tunneled
# through adb -- the same mechanism deploy_apk.sh already used for
# port 6000/8080. adb forward has to be (re)established after every USB
# reconnect/reboot before gps_uplink.py can reach 127.0.0.1:6000, so
# this script does that first and then hands off to the Python script.
#
# Requires: sudo apt install -y android-tools-adb
#
# One-time manual step the first time you run this against a given hub:
# adb will show an "Allow USB debugging?" prompt ON THE HUB'S OWN SCREEN.
# Tap it and check "always allow from this computer" -- there is no way
# to automate that confirmation, and it only needs doing once per hub
# (unless the hub gets factory reset).
#
# IMPORTANT: run adb consistently as this same non-root user (orangepi),
# never with sudo. Mixing sudo and non-sudo adb invocations creates two
# separate adb servers owned by different users, which is what forces
# the "sudo pkill -9 adb" workaround. Keeping everything as one user
# avoids that entirely.

set -uo pipefail

PORT="${1:-/dev/ttyUSB2}"

# Force a clean adb server every run. A root-owned adb process
# (traced to the board's usbdevice/adbc process -- likely Rockchip's
# USB gadget/FunctionFS support, separate from our own adb install)
# reappears at boot and survives a plain non-sudo "adb kill-server",
# since a user-level kill can't touch a root-owned process. The sudo
# pkill below requires the /etc/sudoers.d/adb-pkill rule to be in
# place (passwordless, scoped to exactly this command) since this
# script runs headless under systemd with no TTY for a password
# prompt.
echo "==> Clearing any root-owned adb process"
sudo pkill -9 adb 2>/dev/null || true
sleep 1

echo "==> Resetting adb server"
adb kill-server 2>/dev/null || true
sleep 1

echo "==> Starting adb server"
adb start-server

echo "==> Waiting for the hub over USB"
adb wait-for-device

echo "==> Forwarding ports (hub_link TCP + nav telemetry HTTP)"
adb forward tcp:6000 tcp:6000
adb forward tcp:8080 tcp:8080

echo "==> Launching gps_uplink.py"
exec python3 "$(dirname "$0")/gps_uplink.py" --hub-ip 127.0.0.1 --port "$PORT"
