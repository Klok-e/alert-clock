#!/usr/bin/env bash
# Installs the alert-clock gateway on a Raspberry Pi (Raspberry Pi OS / Debian).
# Run from your own account (e.g. pi) with sudo: sudo gateway/install.sh
# The service runs as that account.
set -euo pipefail

GATEWAY_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG=/etc/alert-gateway.toml

if [[ $EUID -ne 0 ]]; then
  echo "run with sudo" >&2
  exit 1
fi
RUN_USER="${SUDO_USER:-}"
if [[ -z $RUN_USER || $RUN_USER == root ]]; then
  echo "run with sudo from the account the gateway should run as (e.g. pi), not as root" >&2
  exit 1
fi

apt-get install -y python3-venv fonts-inter

runuser -u "$RUN_USER" -- python3 -m venv "$GATEWAY_DIR/.venv"
runuser -u "$RUN_USER" -- "$GATEWAY_DIR/.venv/bin/pip" install --quiet -r "$GATEWAY_DIR/requirements.txt"

if [[ ! -f $CONFIG ]]; then
  key="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
  # Created unreadable to others, so the key is never world-readable.
  (umask 077 && sed "s/REPLACE_WITH_64_HEX_CHARACTERS/$key/" "$GATEWAY_DIR/alert-gateway.example.toml" > "$CONFIG")
  chgrp "$(id -gn "$RUN_USER")" "$CONFIG"
  chmod 640 "$CONFIG"
  echo "Wrote $CONFIG. Set [home] lat/lon there, then restart the service."
  echo "Put this line in the clock's include/secrets.h:"
  echo "  #define CLOCK_GATEWAY_KEY \"$key\""
fi

sed -e "s|@GATEWAY_DIR@|$GATEWAY_DIR|g" -e "s|@USER@|$RUN_USER|g" "$GATEWAY_DIR/alert-gateway.service" > /etc/systemd/system/alert-gateway.service
systemctl daemon-reload
systemctl enable --now alert-gateway
systemctl --no-pager status alert-gateway || true
