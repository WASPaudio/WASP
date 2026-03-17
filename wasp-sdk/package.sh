#!/bin/bash
set -e

# Usage: ./package.sh [debug|release] (default: debug)
PROFILE="${1:-debug}"
PROFILE="${PROFILE,,}"

if [ "$PROFILE" != "debug" ] && [ "$PROFILE" != "release" ]; then
    echo "Error: profile must be 'debug' or 'release', got '$PROFILE'"
    exit 1
fi

TARGET="target/wasm32-unknown-unknown/$PROFILE"
STAGING="target/wasp_staging"

package_plugin() {
    local NAME="$1"       # crate output name e.g. synth
    local WASM="$2"       # wasm filename in target dir e.g. synth.wasm
    local MANIFEST="$3"   # path to manifest.json

    local WASM_PATH="$TARGET/$WASM"
    local OUT="../$NAME.wasp"
#    local OUT="$TARGET/$NAME.wasp"

    if [ ! -f "$WASM_PATH" ]; then
        echo "Error: $WASM_PATH not found. Did you build with profile '$PROFILE'?"
        return 1
    fi

    if [ ! -f "$MANIFEST" ]; then
        echo "Error: manifest not found at $MANIFEST"
        return 1
    fi

    rm -rf "$STAGING"
    mkdir -p "$STAGING"
    cp "$WASM_PATH" "$STAGING/dsp.wasm"
    cp "$MANIFEST"  "$STAGING/manifest.json"

    rm -f "$OUT"
    cd "$STAGING"
    zip -r "../../$OUT" .
    cd -

    echo "Packaged [$PROFILE]: $OUT"
}

package_plugin "synth"   "synth.wasm"        "synth/manifest.json"
package_plugin "clipper" "wasp_clipper.wasm" "clipper/manifest.json"