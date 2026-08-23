# Server setup

`middleware.py` is the whole server side. It is a single file, Python 3
standard library only, with ffmpeg as its one external dependency.

It needs to reach two things:

1. **A Stremio streaming server** — the thing that turns a torrent or debrid
   link into an HTTP stream. Usually on port 11470.
2. **The internet** — to query addons for catalogs, streams and subtitles.

And the Vita needs to reach *it*, on port 8480.

---

## 1. Where to run it

Anywhere that has ffmpeg and can see your Stremio server. Pick whichever
matches your setup.

### A. Same machine as Stremio in Docker

The common case. The middleware runs on the host, not inside the container.

```bash
sudo apt install -y ffmpeg python3
mkdir -p ~/vitastremio && cd ~/vitastremio
# copy middleware.py here
python3 middleware.py
```

The container must publish 11470 to the host:

```bash
docker ps --format '{{.Names}}\t{{.Ports}}'
curl -s http://127.0.0.1:11470/settings | head -c 200
```

If that curl returns JSON you are done. If it refuses, the port is not
published — recreate the container with `-p 11470:11470`, or point the
middleware at the container's address:

```bash
docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' CONTAINER
export STREMIO_URL="http://172.17.0.2:11470"
```

### B. Stremio Desktop on a PC or Mac

Stremio Desktop runs the same streaming server internally, on the same port.
Run the middleware on that machine, or on any other machine on the LAN
pointing at it:

```bash
export STREMIO_URL="http://192.168.1.20:11470"
python3 middleware.py
```

On macOS: `brew install ffmpeg python3`. On Windows, use WSL2 — install
ffmpeg inside the WSL distribution, and note that WSL2 needs a port proxy
for the Vita to reach it:

```powershell
netsh interface portproxy add v4tov4 listenport=8480 `
      listenaddress=0.0.0.0 connectport=8480 connectaddress=(wsl hostname -I)
```

### C. A NAS, Raspberry Pi, or separate server

Works, with one caveat: **transcoding is the load**. See §4.

```bash
export STREMIO_URL="http://192.168.1.30:11470"
python3 middleware.py
```

Synology and QNAP can run it in a container or over SSH. A Pi 4 can manage
one 1080p stream with software encoding; a Pi 5 is comfortable.

### D. No Stremio server at all

**This is a supported setup, and probably more common than it looks.**

`STREMIO_URL` is only used for **torrent** sources — the ones an addon
returns as an infoHash. Addons that return resolved links, which is every
debrid configuration, are fetched directly by ffmpeg over HTTPS. Stremio is
never consulted for those.

So if every source in your list shows **INSTANT**, the Stremio server is not
in the path at all, and the config page reporting it as "not used" is
accurate rather than a fault.

Set `STREMIO_URL` to anything; torrent sources will fail and direct ones
will work normally. Install a Stremio server only if you want torrent
support.

---

## 2. Configure addons

Open the web page the middleware serves:

```
http://<server>:8480/
```

Paste an addon's manifest URL, or import everything from your Stremio
account. Both are saved to `~/.vitastremio_addons.json` (mode 600) and
survive restarts.

**Addon URLs are the primary route, and the recommended one.** They are
stable, they work for every addon, and nothing upstream can change them out
from under you.

### Optionally: import a whole account at once

If you have many addons configured in Stremio, you can import them in one
step with an auth key:

1. Sign in at `app.strem.io` in a browser
2. Open developer tools, then Application -> Local Storage -> `app.strem.io`
3. Copy the `authKey` value
4. Paste it into **Import with an auth key** on the config page

The key is stored at mode 600, your addons are imported, and the list
re-syncs periodically so installing something in Stremio picks it up here.
Equivalent from a terminal:

```bash
python3 middleware.py --auth-key "PASTE_IT_HERE"
```

**There is deliberately no email and password login.** Stremio's login
endpoint is undocumented and its response shape has changed more than once;
supporting it meant guessing at request formats and failing opaquely when
they moved. An auth key does the same job without that fragility, and works
for accounts created through Facebook, Google or Apple, which have no
password at all.

Anything on the Stremio addon protocol works — Cinemeta, Torrentio,
AIOStreams, Comet, MediaFusion, and so on.

**Keep a catalog provider.** Stream aggregators supply no catalogs, so on
their own you get an empty grid. Cinemeta is added automatically if nothing
else provides catalogs.

If you prefer environment variables, they override the saved list:

```bash
export ADDONS="https://v3-cinemeta.strem.io/manifest.json,https://<your-addon>/manifest.json"
```

---

## 3. Verify before touching the Vita

The whole chain is testable with curl. From a **different** machine:

```bash
curl -s http://SERVER:8480/ping                      # -> ok
```

From the server:

```bash
curl -s 'http://localhost:8480/catalog?type=movie' | head -3 | cat -v
curl -s 'http://localhost:8480/streams?type=movie&id=tt0133093' | head -3 | cat -v
```

Fields are separated by `^_`. Empty stream output means no stream addon is
configured. Stream lookups against an aggregator can take 30–60 seconds on a
cold cache; that is normal.

If `/ping` times out from another machine, open the port:

```bash
sudo ufw allow from 192.168.0.0/16 to any port 8480 proto tcp
```

---

## 4. Transcoding and hardware acceleration

Every stream is transcoded to 960x544 H.264 plus PCM. This is the only
significant CPU cost.

**Intel Quick Sync** (recommended), enabled automatically when available:

```bash
vainfo | grep -i h264
ls -l /dev/dri/renderD128
sudo usermod -aG render $USER     # then log out and back in
```

The middleware prints `VAAPI encode OK` at startup when this works. One
stream is a rounding error on any Quick Sync capable chip.

**Software encoding** is the fallback and works fine — a modern desktop core
handles one 960x544 stream comfortably. Watch for the middleware warning
that VAAPI failed; if `intel_gpu_top` shows the video engine idle while a
core is pegged, it silently fell back.

**AMD and NVIDIA** are not wired up. VAAPI on AMD would likely work by
changing `VAAPI_DEVICE`; NVENC would need the ffmpeg arguments changed in
`video_cmd()`. Both are small changes if you want them.

**Prefer 1080p sources.** Everything is scaled to 960x544 regardless, so a
4K remux costs enormous bandwidth and decoding for an identical picture.

---

## 5. Run it permanently

The `export` lines only live in one shell. For a machine that should just
work when you pick up the Vita:

```bash
sudo tee /etc/systemd/system/vitastremio.service >/dev/null <<'EOF'
[Unit]
Description=vitastremio middleware
After=network-online.target docker.service

[Service]
Type=simple
User=YOUR_USER
WorkingDirectory=/home/YOUR_USER/vitastremio
Environment=STREMIO_URL=http://127.0.0.1:11470
ExecStart=/usr/bin/python3 /home/YOUR_USER/vitastremio/middleware.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now vitastremio
journalctl -u vitastremio -f
```

Addons come from the saved config, so no secrets belong in this file.

---

## 6. Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `STREMIO_URL` | `http://127.0.0.1:11470` | Stremio streaming server |
| `VITA_PORT` | `8480` | port the Vita connects to |
| `ADDONS` | saved config | comma-separated manifest URLs; overrides the web config |
| `VAAPI_DEVICE` | `/dev/dri/renderD128` | GPU render node |
| `MAX_TRANSCODES` | `6` | concurrent ffmpeg cap |
| `STREAM_TIMEOUT` | `75` | seconds to wait for an addon's stream list |
| `MAX_STREAMS` | `40` | sources returned per title |
| `POSTER_TIMEOUT` | `25` | seconds for artwork |
| `SYNC_INTERVAL` | `900` | seconds between Stremio account syncs |
| `VITA_ADDONS_FILE` | `~/.vitastremio_addons.json` | saved addon list |
| `VITA_AUTH` | `~/.vitastremio_auth` | cached Stremio auth key |

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Vita says "no server at …" | wrong address, firewall, not running | `curl http://SERVER:8480/ping` from a third machine |
| Grid is empty | no catalog addon | add Cinemeta on the config page |
| Every title shows "0 sources" | no stream addon | add Torrentio, AIOStreams or similar |
| Sources take a minute | aggregator cold cache | normal; `STREAM_TIMEOUT` controls the limit |
| Stutter, `under` climbing in stats | network cannot keep up | pick a 1080p source, or reduce `AUD_CH` to 1 |
| CPU pegged during playback | VAAPI fell back to software | check `vainfo` and `/dev/dri` group membership |
| Some posters never load | addon supplied no artwork | check the middleware log; it names each failure |
| Subtitles empty | no subtitle addon | OpenSubtitles v3 is queried automatically; check connectivity |

The middleware logs every request and every failure with a reason. It is the
first place to look.
