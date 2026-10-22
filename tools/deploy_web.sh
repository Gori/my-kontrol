#!/bin/sh
# Deploys the web installer with the current M32 build to my-kontrol.vercel.app.
# Needs the Vercel CLI (logged in) and the Gaisyr fonts in web/fonts (not in git).
set -e
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/.*MK_VERSION "\(.*\)"/\1/p' firmware/core/version.h)
BIN=firmware/build/m32/my-kontrol-m32-$VERSION.bin
[ -f "$BIN" ] || { echo "build first: cd firmware && make"; exit 1; }
ls web/fonts/*.woff2 >/dev/null
OUT=$(mktemp -d)/my-kontrol
mkdir -p "$OUT/firmware/m32"
rsync -a --exclude test --exclude package.json --exclude README.md --exclude firmware web/ "$OUT/"
cp web/firmware/manifest.json "$OUT/firmware/"
WEB_ROOT="$OUT" python3 tools/publish_web.py m32 "$BIN" "$VERSION" "${1:-$(git log -1 --format=%s)}"
cd "$OUT" && vercel deploy --prod --yes
