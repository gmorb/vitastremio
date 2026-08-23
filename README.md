# vitastremio

**v1.0 beta**

A native PlayStation Vita client for Stremio, with a companion server that
does everything the Vita cannot.

Browse your Stremio catalogs, search, pick a source, and watch — on a
handheld from 2011, with hardware-decoded video, working audio, subtitles,
audio track selection and touch controls.

![status](https://img.shields.io/badge/status-beta-orange)
![platform](https://img.shields.io/badge/platform-PS%20Vita-blue)

---

## What this actually is

The Vita cannot run Stremio. It has 512MB of RAM, a 444MHz ARM CPU, a
hardware video decoder that only understands H.264, and a TLS stack from
2011 that modern addon servers reject. Nothing about the Stremio ecosystem
fits on it.

So the work is split:

```
Stremio addons (HTTPS)
        |
Stremio server (:11470)          torrent / debrid -> HTTP
        |
middleware.py (:8480)            addon protocol, metadata, transcode
        |  raw H.264 (960x544, CFR, no B-frames)
        |  raw PCM  (32kHz stereo)
        v
PS Vita                          sceAvcdec + sceAudioOut
```

The Vita never parses JSON, never negotiates TLS, and never demuxes a
container. It receives two raw elementary streams and draws a UI. Every hard
problem lives on the server, which has a GPU encoder and gigabytes of RAM.

**You need a machine to run the middleware on.** It does not have to be the
same machine as your Stremio server, and it does not have to be Docker — see
[docs/SERVER.md](docs/SERVER.md).

---

## Features

- Browse and search Stremio catalogs
- Source list with resolution, size, language and instant-availability
- Hardware H.264 playback at 960x544, frame-accurate pacing
- Subtitles from Stremio's subtitle addons (SRT and WebVTT)
- Audio track selection for multi-language releases
- Touch and D-pad controls, seek bar, pause, ±30s skip
- On-screen A/V sync trim, saved between sessions
- Server address configurable on the device; no rebuild to move servers
- Web configuration page: add addons by URL, or import an account with an auth key

---

## Quick start

**1. Server** — on any Linux machine that can reach your Stremio server:

```bash
sudo apt install -y ffmpeg python3
python3 middleware.py
```

Then open `http://<that-machine>:8480/` in a browser and add your addons.

**2. Vita** — install `release/vitastremio-v1.0-beta.vpk` with VitaShell,
launch it, and enter the server's address when prompted.

Full instructions: [docs/SERVER.md](docs/SERVER.md) and
[docs/BUILD.md](docs/BUILD.md).

---

## Repository layout

```
src/          Vita client (C, vitasdk + vita2d)
server/       middleware.py -- the whole server side, stdlib only
test/         host-side tests; no Vita toolchain needed
docs/         setup and build guides
release/      prebuilt .vpk
```

### The client

| File | Purpose |
|---|---|
| `main.c` | UI, input, screens, background worker |
| `player.c/.h` | decode, audio, A/V sync, frame pacing |
| `http.h` | minimal HTTP over raw sockets |
| `annexb.h` | H.264 access unit splitter |
| `ring.h` | lock-free SPSC ring buffer |
| `lineproto.h` | wire format parser |
| `config.h` | server address parsing and persistence |
| `ime.h` | on-screen keyboard |
| `log.h` | file logger |

---

## Tests

The Vita sources cannot be compiled without vitasdk, so the parts that are
pure data handling are testable on any Linux box — along with a type-check
of the real sources against stub headers:

```bash
cd test && make
```

This covers declaration order, a full compile of `main.c` and `player.c`,
the Annex-B splitter, the address parser, and the ring buffer under
ThreadSanitizer. Run it before opening a PR.

---

## Known limitations

- **Subtitles are Latin-1.** The Vita's system font has no Cyrillic, Greek,
  Arabic or CJK glyphs, so those render as substitution characters.
- **A/V offset needs one-time tuning.** The default suits most sources; the
  in-app trim handles the rest.
- **Seeking restarts the transcode.** Roughly a two second gap. It needs no
  index and no discontinuity handling, which is why it works at all.
- **One stream at a time per middleware.** Two Vitas need two ports.
- **4K sources are wasteful.** Everything is scaled to 960x544; a 1080p
  source gives an identical picture for a fraction of the bandwidth.

---

## Credits

Built with [vitasdk](https://vitasdk.org) and
[vita2d](https://github.com/xerpi/libvita2d). Not affiliated with Stremio.

## Licence

MIT — see [LICENSE](LICENSE).
