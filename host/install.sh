#!/usr/bin/env bash
# Desk Pet host installer — run as root:  sudo bash host/install.sh
# - Installs a udev rule so the normal user can open /dev/ttyACM0 (flash + runtime)
# - Adds the user to the dialout group (takes effect next login)
# - Installs + enables the systemd service for auto-start on boot
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RULE_SRC="$HERE/99-deskpet.rules"
UNIT_SRC="$HERE/deskpet.service"
RULE_DST=/etc/udev/rules.d/99-deskpet.rules
UNIT_DST=/etc/systemd/system/deskpet.service

echo "[1/4] Installing udev rule -> $RULE_DST"
install -m 0644 "$RULE_SRC" "$RULE_DST"
udevadm control --reload-rules
udevadm trigger 2>/dev/null || true

echo "[2/4] Adding user 'humjie' to 'dialout'"
usermod -aG dialout humjie || true

echo "[3/4] Installing systemd unit -> $UNIT_DST"
install -m 0644 "$UNIT_SRC" "$UNIT_DST"
systemctl daemon-reload

echo "[4/4] Enabling deskpet.service (auto-start on boot)"
systemctl enable deskpet.service

echo
echo "Done. Start it with:   sudo systemctl start deskpet"
echo "Check it with:         systemctl status deskpet   (or: journalctl -u deskpet -f)"
echo "Note: dialout membership applies to NEW login sessions; udev rule works immediately."