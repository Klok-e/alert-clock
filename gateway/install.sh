#!/usr/bin/env bash
# Installs the alert-clock gateway on a Raspberry Pi (Raspberry Pi OS / Debian).
# Run from the gateway/ directory of a checkout: sudo ./install.sh
set -euo pipefail

GATEWAY_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG=/etc/alert-gateway.toml

if [[ $EUID -ne 0 ]]; then
  echo "run with sudo" >&2
  exit 1
fi

apt-get install -y python3-venv fonts-inter
id alert-gateway >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin alert-gateway

if ! runuser -u alert-gateway -- test -r "$GATEWAY_DIR/alert_gateway/__main__.py"; then
  echo "the alert-gateway user cannot read $GATEWAY_DIR; clone the repository under /opt" >&2
  exit 1
fi

python3 -m venv "$GATEWAY_DIR/.venv"
"$GATEWAY_DIR/.venv/bin/pip" install --quiet -r "$GATEWAY_DIR/requirements.txt"

if [[ ! -f $CONFIG ]]; then
  key="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
  # Created unreadable to others, so the key is never world-readable.
  (umask 077 && sed "s/REPLACE_WITH_64_HEX_CHARACTERS/$key/" "$GATEWAY_DIR/alert-gateway.example.toml" > "$CONFIG")
  chgrp alert-gateway "$CONFIG"
  chmod 640 "$CONFIG"
  echo "Wrote $CONFIG. Set [home] lat/lon there, then restart the service."
  echo "Put this line in the clock's include/secrets.h:"
  echo "  #define CLOCK_GATEWAY_KEY \"$key\""
fi

sed "s|@GATEWAY_DIR@|$GATEWAY_DIR|g" "$GATEWAY_DIR/alert-gateway.service" > /etc/systemd/system/alert-gateway.service
systemctl daemon-reload
systemctl enable --now alert-gateway
systemctl --no-pager status alert-gateway || true
