#!/usr/bin/env bash
# One-shot push to a new GitHub repository.
#
#   ./PUSH.sh YOUR-GITHUB-USERNAME [repo-name]
#
# Create the repository on github.com first, EMPTY -- no README, licence or
# .gitignore. This tree already has all three and would conflict with them.
set -euo pipefail

USER="${1:-}"
NAME="${2:-vitastremio}"

if [ -z "$USER" ]; then
    echo "usage: $0 YOUR-GITHUB-USERNAME [repo-name]" >&2
    exit 1
fi

cd "$(dirname "$0")"

# Refuse to publish anything that looks personal. Cheap, and the one thing
# that is genuinely awkward to undo once it is public.
PATTERN='aiostreams\.thegoat|thegoattechnician|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-'

echo "== checking for personal data =="
# --exclude the script itself: the patterns below would otherwise match the
# grep that looks for them.
if grep -rniE "$PATTERN" . \
     --exclude-dir=.git --exclude-dir=release --exclude="$(basename "$0")" \
     2>/dev/null; then
    echo "!! found something that looks personal -- review before pushing" >&2
    exit 1
fi
echo "   clean"

echo "== running tests =="
make -C test >/dev/null && echo "   passed"
make -C test clean >/dev/null 2>&1 || true

echo "== pushing =="
[ -d .git ] || git init -q
git add .
git commit -q -m "vitastremio v1.0 beta" || echo "   nothing new to commit"
git branch -M main
git remote remove origin 2>/dev/null || true
git remote add origin "https://github.com/$USER/$NAME.git"
git push -u origin main

git tag -f v1.0-beta -m "v1.0 beta" >/dev/null
git push -f origin v1.0-beta

echo
echo "Done: https://github.com/$USER/$NAME"
echo "To attach the vpk as a downloadable release:"
echo "  Releases -> Draft a new release -> tag v1.0-beta -> attach"
echo "  release/vitastremio-v1.0-beta.vpk -> mark as pre-release"
