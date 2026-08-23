# Changelog

## v1.0 beta

First release.

### Client
- Catalog browsing, search, and a source list showing resolution, size,
  language and instant availability
- Hardware H.264 playback at 960x544, vsync-locked frame pacing
- Subtitles via Stremio subtitle addons (SRT and WebVTT)
- Audio track selection for multi-language releases
- Touch and D-pad controls, seek bar, pause, +/-30s skip
- Server address set on the device and persisted; no rebuild to move servers
- Diagnostics overlay with cadence, sync and draw-call counters
  (SELECT + TRIANGLE)

### Server
- Addon protocol client: catalogs, search, streams, subtitles
- VAAPI transcode with software fallback
- Web configuration page, or Stremio account import with periodic sync
- Poster fetching with caching and a fallback path

### Notable fixes during development
- Frame pacing driven by the vblank counter rather than the audio clock,
  eliminating irregular cadence that read as stutter
- Panel refresh rate measured against the audio clock rather than assumed,
  removing lip-sync drift
- Persistent buffer pool: no GPU allocation during stream changes
- Correct unwinding of decoder resources on every failure path
