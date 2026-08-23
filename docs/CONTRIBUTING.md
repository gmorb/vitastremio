# Contributing

## Before opening a PR

```bash
cd test && make
```

That runs four things, in order of how much time they save:

1. **Declaration order** — catches use-before-definition and references to
   struct members that no longer exist. Both are invisible until the Vita
   toolchain rejects them.
2. **Host type-check** — compiles `main.c` and `player.c` against stub
   headers with `-Werror` on the diagnostics that indicate real mistakes.
3. **Annex-B splitter** — including one-byte-at-a-time feeding, which is
   what TCP actually does.
4. **Ring buffer** — under ThreadSanitizer.

The stubs in `test/stubs/` approximate the vitasdk API well enough to catch
signature errors. They are not accurate reproductions and must never be
linked against. If something type-checks there but fails on real vitasdk,
the stub is what needs correcting.

## Things worth knowing

**Frame pacing is deliberate.** The video schedule runs off the hardware
vblank counter, not the audio clock, because the audio clock jitters by up
to one grain and that is enough to flip a 2-refresh frame into a 3-refresh
one. The average stays correct while the cadence becomes irregular, which is
exactly what stutter is. See the comments in `vs_play_present`.

**Buffers are allocated once.** Frame textures and ring buffers live for the
life of the process. Allocating GPU memory during a stream change maps
memory into the GPU MMU while it is rendering, which faulted the GPU
reliably. Do not reintroduce per-session allocation.

**The client parses nothing complicated.** No JSON, no TLS, no containers.
If a change requires the Vita to understand a new format, it probably
belongs in the middleware instead.

**Primitive count matters.** vita2d packs a frame's geometry into a fixed
pool without bounds-checking it. `SELECT + TRIANGLE` shows the running
count and peak.
