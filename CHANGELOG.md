# Changelog

All notable changes to vitastremio (PS Vita Stremio client + middleware).

## [1.2] - 2026-08-27

### Fixed

**Video dropping to a slideshow.** After a stall, playback could fall to
roughly one frame per second and stay there while the audio ran on normally.
Correcting a large sync gap takes a few seconds to play out, and the player
was re-correcting before the previous attempt had finished -- so it re-drew
once a second instead of catching up. It now lets a correction complete
before making another.

**Sound and picture drifting apart while buffering.** The player was trying
to correct sync during a stall, when playback is deliberately held still and
the measurement means nothing. It now waits until playback resumes, which is
when the gap is real and worth closing.

### Changed

- Playback log messages now report which of sound or picture is ahead. They
  previously described both cases the same way, which made a stalling audio
  stream look like a slow video connection.
- When a stream starts from the beginning unexpectedly, the log now records
  whether a saved resume position was found, so the cause can be identified.
