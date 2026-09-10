# ClimateControl server

The building server for the ClimateControl HVAC system. It accepts one TCP
connection from the `MainController`, decodes the `NodeLib` v2 frame stream into
a SQLite history, and serves the operator web application (a single-page app
over HTTP + WebSocket).

Full protocol design: `../Spec/MainController-Server-Link-Spec.md`.

```
MainController ──TCP(:9000)──▶ ccserver ──┬─ SQLite (history + config)
                                          └─ HTTP(:8080): SPA + JSON API + /ws ──▶ browsers
```

- **LAN only, no TLS.** The `MainController` authenticates with a 16-byte token.
- **One process, one binary**, the web UI is embedded. No external runtime.
- **SQLite**, one file. Back it up by copying it.

---

## 1. What you need

- A small always-on Linux box on the same LAN as the `MainController` — a
  Raspberry Pi (3/4/5, 64-bit OS), an Intel NUC, or any mini-PC. Ubuntu Server
  22.04 or 24.04 LTS is the reference target; any recent systemd distro works.
- ~50 MB disk for the binary, plus history growth (a few MB/month for a
  typical building).
- Two free TCP ports: **8080** (web UI) and **9000** (MainController uplink).
  Both are configurable.

You do **not** need Go on the server if you deploy a prebuilt binary (§3). You
need Go 1.27+ only to build one.

---

## 2. Build the binary

### On a build machine (recommended: cross-compile, then copy)

```sh
cd Webserver

# for a 64-bit ARM box (Raspberry Pi 3/4/5 with a 64-bit OS):
make build-arm64        # -> bin/ccserver-linux-arm64

# for a 32-bit ARM box (older Pi / 32-bit OS):
make build-armv7        # -> bin/ccserver-linux-armv7

# for an x86-64 box (NUC / mini-PC / VM):
make build-amd64        # -> bin/ccserver-linux-amd64
```

The build is static (`CGO_ENABLED=0`) — the resulting file has no shared-library
dependencies and runs on any Linux of that architecture.

### Or build on the server itself

```sh
sudo apt update && sudo apt install -y golang-go git   # or a newer Go from https://go.dev/dl/
cd Webserver
make build                                             # -> bin/ccserver
```

If Ubuntu's packaged Go is older than 1.27, install the current toolchain:

```sh
curl -fsSL https://go.dev/dl/go1.27.1.linux-amd64.tar.gz | sudo tar -C /usr/local -xz
export PATH=$PATH:/usr/local/go/bin
```

---

## 3. Install on the Ubuntu server

Run these on the server. Replace `<arch>` with `amd64`, `arm64`, or `armv7`.

```sh
# 3.1 dedicated service user (no login, no home)
sudo useradd --system --no-create-home --shell /usr/sbin/nologin ccserver

# 3.2 install directory
sudo mkdir -p /opt/ccserver/data
sudo cp ccserver-linux-<arch>        /opt/ccserver/ccserver
sudo cp deploy/config.example.json   /opt/ccserver/config.json
sudo cp deploy/ccserver.service       /etc/systemd/system/ccserver.service
sudo chown -R ccserver:ccserver /opt/ccserver
sudo chmod 750 /opt/ccserver
sudo chmod +x  /opt/ccserver/ccserver

# 3.2b if :8080 or :9000 is already taken on this box (e.g. another web app),
#      edit /opt/ccserver/config.json now and set httpAddr / uplinkAddr to
#      free ports — check with:  sudo ss -ltnp

# 3.3 first start — the empty uplinkToken in config.json is filled in with a
#     fresh 16-byte secret and written back
sudo systemctl daemon-reload
sudo systemctl enable --now ccserver

# 3.4 read the generated token (you need it for the MainController)
sudo -u ccserver cat /opt/ccserver/config.json
```

`config.json` after the first run:

```json
{
  "httpAddr": ":8080",
  "uplinkAddr": ":9000",
  "uplinkToken": "3f9c1a...<32 hex characters>",
  "dataDir": "/opt/ccserver/data"
}
```

| field | meaning |
|---|---|
| `httpAddr` | listen address for the web UI + API. `:8080` = all interfaces. Use `127.0.0.1:8080` if a reverse proxy fronts it. |
| `uplinkAddr` | listen address for the `MainController` TCP connection. |
| `uplinkToken` | the 32-hex-char shared secret. Generated once; keep it. The `MainController` must send it in its first frame. |
| `dataDir` | holds `climatecontrol.db` and uploaded floor-plan / firmware files. |

Edit the file and `sudo systemctl restart ccserver` to apply changes.

### 3.5 Firewall

If `ufw` is enabled, allow the two ports on the LAN only:

```sh
sudo ufw allow from 192.168.0.0/16 to any port 8080 proto tcp
sudo ufw allow from 192.168.0.0/16 to any port 9000 proto tcp
```

Adjust the subnet to your LAN. Do **not** expose either port to the internet —
the uplink has no TLS.

### 3.6 Check it

```sh
systemctl status ccserver
journalctl -u ccserver -f          # live logs
curl -s http://localhost:8080/api/health
```

Open `http://<server-ip>:8080/` in a browser on the LAN.

---

## 4. Point the MainController at it

The `MainController` firmware needs two values (see
`../Spec/MainController-Server-Link-Spec.md` §3 / §5):

- **server address** — `<server-ip>:9000`
- **uplink token** — the 32-hex string from `config.json`

Set these however the firmware exposes them (compile-time config, provisioning
record, or a local settings interface — TBD in firmware). Once connected, the
server logs `uplink connected` and nodes appear on the Status page within a poll
cycle.

---

## 5. Day-to-day

**Web UI** — `http://<server-ip>:8080/`

| Page | Use |
|---|---|
| Map | live floor-plan heatmap of room temperatures |
| Overrides | change a setpoint / damper mode; the value is held and re-applied if the node reboots |
| Status | per-node health, bus counters, and firmware upload |
| Map setup | upload a floor-plan image, click to place each ControllerNode |

**Firmware update** — Status page → pick a node → upload the module's `.bin`
(`ControllerNode`, `TemperatureNode`, or MainController image from
`Software/build/Modules/*`). The server validates the image header and CRC,
then drives the OTA sequence; progress shows in the job table.

**Backup** — the entire state is one SQLite file:

```sh
sudo systemctl stop ccserver
sudo cp /opt/ccserver/data/climatecontrol.db  /path/to/backup/
sudo systemctl start ccserver
```

(Or copy it live — WAL mode makes a hot copy safe enough for this data.)
Uploaded floor plans and firmware images live under `/opt/ccserver/data/assets/`.

**Upgrade** — replace the binary and restart:

```sh
sudo systemctl stop ccserver
sudo cp ccserver-linux-<arch> /opt/ccserver/ccserver
sudo systemctl start ccserver
```

The schema migrates itself on start. Downgrades are not supported — back up the
DB first.

---

## 6. Optional: reverse proxy + hostname

For a nicer URL (`http://climate.lan/`) or LAN TLS, front it with nginx or
Caddy. Set `httpAddr` to `127.0.0.1:8080` first. The `/ws` endpoint needs
WebSocket upgrade headers passed through:

```nginx
# /etc/nginx/sites-available/ccserver
server {
    listen 80;
    server_name climate.lan;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 1h;
    }
}
```

The uplink port (9000) is a raw TCP socket — it is **not** proxied and stays a
direct LAN connection.

---

## 7. Development

```sh
cd Webserver
make test                       # go vet + unit tests (frame codec, decoders)
make run                        # builds, runs against ./data, creates ./config.json

# in a second terminal — a fake MainController that streams plausible readings:
make sim                        # reads the token from ./config.json
# or explicitly:
go run ./cmd/simnode -addr 127.0.0.1:9000 -token <token> -controllers 3 -temps 1
```

Then open `http://localhost:8080/`.

### Layout

```
Webserver/
├── cmd/ccserver/     server entry point
├── cmd/simnode/      MainController simulator (dev only)
├── internal/
│   ├── nodelib/      frame codec (CRC16, deframer, encoder) + endpoint decoders + image parser
│   ├── uplink/       TCP listener, token auth, frame dispatch, downlink queue
│   ├── service/      wires uplink -> store + hub; OTA driver
│   ├── store/        SQLite: nodes, readings, commands, overrides, ota_jobs, map_*
│   ├── hub/          in-memory current-state cache + WebSocket fan-out
│   ├── ws/           minimal RFC 6455 server
│   ├── httpapi/      routes: SPA bundle, JSON API, /ws
│   └── config/       config.json load + token generation
├── web/              the SPA (index.html, app.css, app.js — embedded at build)
└── deploy/           systemd unit + example config
```

The `nodelib` package mirrors `Software/Lib/NodeLib` — keep the two in sync when
the wire protocol changes.

### JSON API (summary)

| method + path | purpose |
|---|---|
| `GET /api/health` | liveness |
| `GET /api/state` | full current-state snapshot (same shape as the WS `snapshot`) |
| `GET /api/nodes` | roster |
| `GET /api/readings?node=&endpoint=&from=&to=&limit=` | history series |
| `POST /api/commands` `{node,endpoint,value}` | queue a `Set`; held as an override |
| `GET/DELETE /api/overrides[/{node}/{endpoint}]` | held values |
| `GET/POST/DELETE /api/floors[/{id}]`, `GET /api/floors/{id}/image` | floor plans |
| `GET /api/placements`, `PUT/DELETE /api/placements/{node}` | node map positions |
| `POST /api/ota` (multipart: `node`, `image`), `GET /api/ota[/{id}]` | firmware push |
| `GET /ws` | live channel: `snapshot` then `value` / `presence` / `main` / `ota` events |
