# Stub headers

Minimal stand-ins for the vitasdk and vita2d headers, just enough for a host
compiler to parse and type-check `src/*.c`.

They exist because the real sources cannot otherwise be built without the
Vita toolchain, so syntax errors, wrong function signatures, references to
struct members that no longer exist, and format-string mistakes all used to
surface only as a failed build on the device.

These are NOT accurate reproductions of the real API and must never be linked
against. Signatures are only as precise as needed to catch mistakes on our
side. If a call type-checks here but fails on real vitasdk, the stub is what
needs correcting.
