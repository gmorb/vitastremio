# Changelog

All notable changes to vitastremio (PS Vita Stremio client + middleware).

## [1.1] - 2026-08-27

A stability release. Most of this is about playback holding together when
the connection wobbles.

### Fixed

**Video freezing while the sound kept playing.** The picture would lock up,
audio carried on, and pausing did nothing -- the only way out was seeking or
restarting the stream. This had two separate causes and both are fixed, with
a safety net behind them that recovers within a quarter of a second no matter
what causes it.

**Sound and picture drifting apart after buffering.** A short stall left them
out of step and a longer one left them drifting further and further apart,
with no way back short of restarting. Playback now holds still while it waits
for the connection, so it comes back in step by itself. If a gap does open,
it closes in about a second instead of a minute or not at all.

**Missing bass on surround soundtracks.** On 5.1 sources the low-frequency
channel was being dropped entirely, so anything mixed into it just wasn't
there.

**Dialogue buried under the music and effects.** Also on 5.1 sources, speech
came through quieter than everything around it. It now sits above the rest of
the mix.

**Surround films much quieter than stereo ones.** 5.1 soundtracks played
noticeably quieter than stereo. They are now at matching levels. Stereo and
mono are unchanged -- they were already correct.

**Brief flash of garbage when starting a stream.** A frame of the previous
film, or of nothing in particular, could appear for an instant before
playback began.

**Dropouts on flaky sources.** If a source stopped responding mid-stream the
server would wait indefinitely rather than reconnecting. It now retries.

### Added

**Resume where you left off.** Stop something, come back later, and it picks
up where you stopped. Works even if you choose a different source for the
same episode. Anything under a minute is ignored, and titles you've nearly
finished start fresh.

**A buffering spinner.** When playback pauses to let the connection catch up,
you can now see that's what's happening rather than wondering if it has
frozen.

**A volume boost for headphones.** Films are mixed for cinemas and are often
too quiet on a handheld. With headphones or Bluetooth, this adds extra
volume. Open the overlay with SELECT+TRIANGLE and press SQUARE.

### Notes

- Volume is protected against distortion throughout, including with the
  headphone boost on.
- Correcting a large sync gap is now a visible jump rather than a slow slide.
  That is deliberate -- it is over in an instant instead of taking a minute.
- If your connection can't sustain the stream, video may skip to keep up with
  the sound. That is a bandwidth limit rather than a bug, and a
  lower-quality source will help.
- Resume positions are stored on the computer running the middleware, so they
  don't follow you to a different setup.
- Both parts need updating together: reinstall the VPK **and** restart the
  middleware.
