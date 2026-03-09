#!/bin/bash
set -e

# Usage: ./package.sh [debug|release] (default: debug)
PROFILE="${1:-debug}"
PROFILE="${PROFILE,,}"

if [ "$PROFILE" != "debug" ] && [ "$PROFILE" != "release" ]; then
    echo "Error: profile must be 'debug' or 'release', got '$PROFILE'"
    exit 1
fi

WASM="target/wasm32-unknown-unknown/$PROFILE/wasp_sine.wasm"
MANIFEST="manifest.json"
OUT="target/wasm32-unknown-unknown/$PROFILE/wasp_sine.wasp"
STAGING="target/wasp_staging"

if [ ! -f "$WASM" ]; then
    echo "Error: $WASM not found. Run 'cargo build --target wasm32-unknown-unknown${PROFILE:+ --$PROFILE}' first."
    exit 1
fi

rm -rf "$STAGING"
mkdir -p "$STAGING"
cp "$WASM" "$STAGING/dsp.wasm"
cp "$MANIFEST" "$STAGING/manifest.json"

rm -f "$OUT"
cd "$STAGING"
zip -r "../../$OUT" .
cd -

echo "Packaged [$PROFILE]: $OUT"