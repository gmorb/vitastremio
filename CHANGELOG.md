# Changelog

All notable changes to vitastremio (PS Vita Stremio client + middleware).

## [1.3] - 2026-08-27

**Update both halves.** Reinstall the VPK *and* restart the middleware.

### Fixed

**Sound and picture drifting apart after about 35 minutes.** Long films would
play perfectly and then, always at roughly the same point, the picture would
jump ahead of the sound and never settle again. The app was losing track of
time once playback passed 35 minutes and 47 seconds, which is why it happened
at the same moment every time. It now keeps time correctly for far longer
than any film.

### Added

**A proper loading screen.** Starting something used to show a black screen
while it buffered, which was hard to tell apart from the app having frozen.
You now get the film's artwork, its title, and a spinner while it gets ready.

### Worth knowing

- Artwork comes from your addons, so titles without it will show the title on
  a plain background instead. Nothing else changes.
