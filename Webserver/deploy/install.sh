#!/usr/bin/env bash
#
# Privileged install steps for the ClimateControl building server.
#
# Run this ON THE SERVER, as root:
#
#     sudo ./install.sh
#
# It expects these files next to it (the `make dist` bundle layout, plus an
# optional pre-filled config.json):
#
#     ccserver            the server binary for this machine's architecture
#     ccserver.service    the systemd unit
#     config.json         optional; if absent, config.example.json is used
#     config.example.json fallback config
#
# The script is idempotent: re-run it to upgrade the binary or unit. It never
# overwrites an existing /opt/ccserver/config.json (that file holds the
# generated uplink token).

set -euo pipefail

DEST=/opt/ccserver
UNIT=/etc/systemd/system/ccserver.service
SVC_USER=ccserver
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root (sudo $0)" >&2
	exit 1
fi

[ -f "$SRC/ccserver" ]         || { echo "error: $SRC/ccserver not found" >&2; exit 1; }
[ -f "$SRC/ccserver.service" ] || { echo "error: $SRC/ccserver.service not found" >&2; exit 1; }

CFG_SRC="$SRC/config.json"
[ -f "$CFG_SRC" ] || CFG_SRC="$SRC/config.example.json"
[ -f "$CFG_SRC" ] || { echo "error: no config.json or config.example.json next to the script" >&2; exit 1; }

echo "==> service user"
if id "$SVC_USER" >/dev/null 2>&1; then
	echo "    user '$SVC_USER' already exists"
else
	useradd --system --no-create-home --shell /usr/sbin/nologin "$SVC_USER"
	echo "    created system user '$SVC_USER'"
fi

echo "==> stop running service (if any)"
systemctl stop ccserver 2>/dev/null || true

echo "==> install directory $DEST"
install -d -o "$SVC_USER" -g "$SVC_USER" -m 750 "$DEST" "$DEST/data"

echo "==> binary"
install -o "$SVC_USER" -g "$SVC_USER" -m 755 "$SRC/ccserver" "$DEST/ccserver"

echo "==> config"
if [ -f "$DEST/config.json" ]; then
	echo "    keeping existing $DEST/config.json"
else
	install -o "$SVC_USER" -g "$SVC_USER" -m 640 "$CFG_SRC" "$DEST/config.json"
	echo "    installed $DEST/config.json (from $(basename "$CFG_SRC"))"
fi

echo "==> systemd unit"
install -m 644 "$SRC/ccserver.service" "$UNIT"
systemctl daemon-reload
systemctl enable --now ccserver

sleep 1
echo
systemctl --no-pager --full status ccserver | sed -n '1,12p' || true

HTTP_PORT="$(sed -n 's/.*"httpAddr" *: *"\(.*\)".*/\1/p' "$DEST/config.json" | sed 's/.*://')"
UPLINK_PORT="$(sed -n 's/.*"uplinkAddr" *: *"\(.*\)".*/\1/p' "$DEST/config.json" | sed 's/.*://')"
HTTP_PORT="${HTTP_PORT:-8080}"
UPLINK_PORT="${UPLINK_PORT:-9000}"

echo
echo "==> health check"
if command -v curl >/dev/null && curl -fsS "http://127.0.0.1:${HTTP_PORT}/api/health"; then
	echo
else
	echo "    not answering yet — check: journalctl -u ccserver -e"
fi

echo
echo "==> uplink token (configure this on the MainController):"
sed -n 's/.*"uplinkToken" *: *"\([^"]*\)".*/    \1/p' "$DEST/config.json"

if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "Status: active"; then
	# Derive the server's own IPv4 /24 as the allowed source range. Override
	# with:  LAN_CIDR=10.0.0.0/8 sudo ./install.sh
	if [ -z "${LAN_CIDR:-}" ]; then
		ip4="$(hostname -I | tr ' ' '\n' | grep -E '^(10|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.' | head -n1)"
		LAN_CIDR="${ip4%.*}.0/24"
	fi
	echo
	echo "==> ufw is active — opening ${HTTP_PORT}/tcp and ${UPLINK_PORT}/tcp from ${LAN_CIDR}"
	ufw allow from "$LAN_CIDR" to any port "$HTTP_PORT" proto tcp
	ufw allow from "$LAN_CIDR" to any port "$UPLINK_PORT" proto tcp
	ufw status | grep -E "(^Status:|${HTTP_PORT}|${UPLINK_PORT})"
fi

echo
echo "Done. UI:      http://<server-ip>:${HTTP_PORT}/"
echo "     uplink:   <server-ip>:${UPLINK_PORT}"
echo "     logs:     journalctl -u ccserver -f"
echo "     restart:  systemctl restart ccserver   (after editing $DEST/config.json)"
