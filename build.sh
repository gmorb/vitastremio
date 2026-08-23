#!/usr/bin/env bash
# build.sh -- build vitastremio.vpk
#
#   ./build.sh          configure and build
#   ./build.sh clean
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-build}" = "clean" ]; then
    rm -rf build
    echo "cleaned"
    exit 0
fi

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK is not set. See BUILD.md step 2." >&2
    exit 1
fi

if ! command -v arm-vita-eabi-gcc >/dev/null 2>&1; then
    echo "arm-vita-eabi-gcc not on PATH. Run:" >&2
    echo "  export PATH=\$VITASDK/bin:\$PATH" >&2
    exit 1
fi

if ! grep -q '#define MW_IP_DEFAULT' src/main.c; then
    echo "src/main.c is missing MW_IP_DEFAULT -- wrong directory?" >&2
    exit 1
fi

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "built: $(ls -1 build/*.vpk)"
