# Building vitastremio.vpk

Only needed if you want to modify the client. A prebuilt vpk is in
`release/`.

```
build.sh
CMakeLists.txt
src/main.c         browser UI, background network worker
src/player.c/.h    sceAvcdec video + sceAudioOut audio
src/http.h         raw-socket HTTP client
src/annexb.h       Annex-B access unit splitter
src/lineproto.h    wire format parser
src/config.h       server address parsing + ux0:data/vitastremio.cfg
src/ime.h          on-screen keyboard wrapper
src/ring.h         lock-free SPSC ring buffer
src/log.h          file logger -> ux0:data/vitastremio.log
```

> The app is a client. It does nothing without `middleware.py` running --
> see [SERVER.md](SERVER.md). Client and server are versioned together;
> mixing versions can leave features silently inert.

---

## 1. Host prerequisites

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake git wget curl \
                    python3 pkg-config libssl-dev zlib1g-dev
```

Arch:

```bash
sudo pacman -S --needed base-devel cmake git wget curl python
```

macOS:

```bash
brew install cmake git wget coreutils
```

---

## 2. Install vitasdk

`vdpm` fetches a prebuilt `arm-vita-eabi` toolchain, then a package manager
for Vita libraries.

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh        # toolchain; may need sudo for /usr/local
./install-all.sh              # all vdpm packages, including vita2d
cd ..
```

If `/usr/local` needs root, run those two with `sudo -E` — the `-E` matters,
it preserves `VITASDK`. Or install somewhere you own:

```bash
export VITASDK=$HOME/vitasdk
```

Make both exports permanent; every later step depends on them:

```bash
echo 'export VITASDK=/usr/local/vitasdk'  >> ~/.bashrc
echo 'export PATH=$VITASDK/bin:$PATH'     >> ~/.bashrc
```

Verify — all three must succeed:

```bash
arm-vita-eabi-gcc --version
ls $VITASDK/share/vita.toolchain.cmake
ls $VITASDK/arm-vita-eabi/lib/libvita2d.a
```

If `libvita2d.a` is missing, `install-all.sh` didn't finish. Install what
this project links against directly:

```bash
vdpm vita2d
vdpm libpng
vdpm libjpeg-turbo
vdpm freetype
vdpm zlib
```

---

## 3. Server address

**No longer a build-time setting.** On first launch the app opens the
on-screen keyboard and asks for the address; it's saved to
`ux0:data/vitastremio.cfg` and reused on every later launch. Press `START`
any time to change it — useful if the server moves or you're switching
between home and a hotspot.

Accepted input, all equivalent:

```
192.168.1.10              uses the default port 8480
192.168.1.10:8480
http://192.168.1.10:8480/ pasted URLs are tolerated
```

**Must resolve to a dotted-quad IP.** `http.h` has no DNS resolver, so
hostnames like `myserver.local` are rejected at entry rather than accepted
and then silently failing to connect. Find yours with `ip -4 addr` on the
server.

`MW_IP_DEFAULT` in `src/main.c` is only the value pre-filled into the
keyboard on a fresh install. Editing it saves a little typing, nothing more.

---

## 4. Build

```bash
chmod +x build.sh
./build.sh
```

Or by hand:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Output: `build/vitastremio.vpk`.

If cmake can't find the toolchain, pass it explicitly:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
```

---

## 5. Install on the Vita

Needs HENkaku / h-encore (3.60–3.74) or ensō.

**VitaShell over FTP** — easiest for repeat builds:

1. Open VitaShell, press `SELECT` to start FTP. It shows something like
   `ftp://192.168.1.42:1337`.
2. From the host:
   ```bash
   curl -T build/vitastremio.vpk ftp://192.168.1.42:1337/ux0:/
   ```
3. In VitaShell browse to `ux0:/`, highlight the vpk, press `X`, confirm.

**Over USB** — press `SELECT` twice for USB mode, copy, install.

Reinstalling the same TITLEID (`VSTR00001`) overwrites cleanly; no need to
delete first.

---

## 6. Reading the log

`printf` output is discarded on a retail Vita, so the app writes to
`ux0:data/vitastremio.log`, wiped at each launch. Every decoder error code
lands there. It is the primary debugging tool for this project.

```bash
curl -s ftp://192.168.1.42:1337/ux0:/data/vitastremio.log
```

Tight iteration loop:

```bash
VITA=192.168.1.42
./build.sh && curl -T build/vitastremio.vpk ftp://$VITA:1337/ux0:/ && \
  echo "install + run in VitaShell, then press enter" && read && \
  curl -s ftp://$VITA:1337/ux0:/data/vitastremio.log
```

`psp2shell` or `PrincessLog` give live output, but neither is necessary —
the file log exists so you can diagnose a hang without a debugger attached.

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `arm-vita-eabi-gcc: not found` | PATH not set | `export PATH=$VITASDK/bin:$PATH` |
| cmake: `vita.cmake not found` | `VITASDK` unset or wrong | check `ls $VITASDK/share/vita.cmake` |
| `undefined reference to vita2d_*` | vita2d not installed | `vdpm vita2d` |
| `undefined reference to sceAvcdec*` | stub missing from link line | confirm `SceVideodec_stub` in CMakeLists.txt |
| `undefined reference to png_*` / `jpeg_*` | link order | keep `vita2d` **before** `png jpeg z` |
| Builds, black screen at launch | early failure | read `ux0:data/vitastremio.log` |
| "network init failed" | wifi off | connect wifi (the NET sysmodule load is already in `vs_net_init`) |
| "no middleware at …" | wrong IP, firewall, not running | `curl http://SERVER:8480/ping` from a third machine |
| Grid loads, video black | the decoder — known unknown | check the log for `decode 0x…` |
| `undefined reference to sceImeDialog*` | IME stub separate in your vitasdk | add `SceIme_stub` to `target_link_libraries` |
| Keyboard appears frozen | dialog not being pumped | `vita2d_common_dialog_update()` must run every frame it's up |
| "bad address" after typing | not a dotted quad | use the IP, not a hostname |
| Search returns 0 results | old `middleware.py` | the `/search` endpoint is new — update and restart it |

### Firewall

The middleware binds `0.0.0.0`, but Ubuntu may still block the port:

```bash
sudo ufw allow from 192.168.0.0/16 to any port 8480 proto tcp
```

Test from a *different* machine, not the server itself:

```bash
curl -sv http://SERVER_IP:8480/ping
```

### Stub naming

vitasdk occasionally renames stub libraries between releases. If a link
fails on a `Sce*_stub` that clearly exists, check what's actually there and
update `target_link_libraries` in `CMakeLists.txt`:

```bash
ls $VITASDK/arm-vita-eabi/lib/ | grep -i videodec
```

---

## 8. What to expect on first run

Realistically, not a working video player. The likely sequence:

1. App launches, log shows the start line. ✅
2. Grid renders with placeholder rectangles. ✅
3. Posters fill in one at a time. ✅
4. Press `X` — stream list appears. ✅
5. Press `X` again — probably a black screen and `decode 0x…` in the log. ⚠️

Steps 1–4 exercise networking, threading, the wire protocol and the UI. If
those work, the only thing left is the decoder, and the logged error code
tells you which failure you've hit.

The most likely one: `player.c` sets `pic.frame.pixelType = 0` hoping for
RGBA8888 output. If frames decode but render as garbage, the decoder is
emitting YUV420 and needs a conversion step.

### Controls

```
D-pad     move selection
X         select / play
O         back
L / R     page (catalog) · seek ±30s (playback)
Square    search
Triangle  back to browsing (clears the search)
START     change server address
```

Search runs against whichever of your addons advertise search support in
their manifest, with results deduplicated by id. Cinemeta covers most
titles; if a search returns nothing, that addon likely doesn't offer search
for the requested type rather than the title not existing.
