# Changelog

All notable changes to vitastremio (PS Vita Stremio client + middleware).

## [1.01-beta] - 2026-08-25

### Added
- **TV series support**: selecting a series opens a new episode picker screen
  (up to 400 episodes, `S1 E1`-style badges, touch + D-pad, L/R paging),
  backed by the middleware `/meta` endpoint.
- **Browse overlay** (TRIANGLE on the catalog): Movies / Series tabs plus a
  genre list (Action, Comedy, Sci-Fi, Horror, and 15 more). Selection reloads
  the catalog filtered by type and genre; the header shows the active mode.
- Search now respects the active content type (movies or series).
- Subtitles are fetched for the exact episode being played rather than the
  series entry.

- Middleware: pluggable video encoder selection. Probes VAAPI (Intel/AMD),
  VideoToolbox (Apple) and x264 in that order at startup and uses the first
  that works; `VITA_ENCODER=vaapi|videotoolbox|x264` forces one, and
  `X264_PRESET` tunes the software encoder (default `veryfast`).
- Middleware: the config page transcoding indicator now names the active
  encoder instead of only distinguishing hardware from software.

### Changed
- Removed the Stremio server / account status indicators from the middleware
  config page.

## [1.00-beta] - earlier development

### Added
- Runtime server address configuration on the device (START opens an
  ip:port dialog, persisted to `ux0:data/vitastremio.cfg`) -- no rebuild
  needed to change servers.
- On-screen keyboard (SceIme) for address entry and catalog search.
- Hardware-accelerated AVC playback with A/V sync, seek, pause, audio track
  and subtitle selection, auto-hiding touch transport bar.
- Poster grid catalog with lazy artwork loading and off-screen eviction.
- Python middleware translating Stremio addon JSON into a compact line
  protocol, with transcode support and stream label parsing.
