#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build.sh — Cross-compile IDOOM-FX for Ableton Move (aarch64) and package it.
# Produces: dist/dsp.so and idoom-fx-v<version>-module.tar.gz
#
# Uses Docker automatically unless CROSS_PREFIX is set (e.g. on an ARM host
# or inside WSL with gcc-aarch64-linux-gnu installed).
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

cd "$(dirname "$0")"

MODULE_ID="idoom-fx"
IMAGE_NAME="idoom-fx-builder"
VERSION=$(grep '"version"' module.json | sed 's/.*"version": *"\([^"]*\)".*/\1/')
ARCHIVE="${MODULE_ID}-v${VERSION}-module.tar.gz"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f /.dockerenv ]; then
    echo "=== IDOOM-FX build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" .
    fi
    HOST_PWD="$(pwd)"
    case "$(uname -s)" in
        MINGW*|MSYS*) HOST_PWD="$(pwd -W)" ;;   # Git Bash: use Windows path
    esac
    MSYS_NO_PATHCONV=1 docker run --rm -v "${HOST_PWD}:/build" -w /build "$IMAGE_NAME" \
        bash -c "CROSS_PREFIX=aarch64-linux-gnu- ./build.sh"
    exit 0
fi

echo "Compiling ${MODULE_ID} v${VERSION} (${CROSS_PREFIX}gcc)..."
mkdir -p dist
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC -Wall -Wextra \
    dsp/idoom.c \
    -o dist/dsp.so \
    -Ivendor

file dist/dsp.so

# Package: <id>/module.json + <id>/dsp.so
TMP_DIR=$(mktemp -d)
mkdir -p "${TMP_DIR}/${MODULE_ID}"
cp module.json "${TMP_DIR}/${MODULE_ID}/"
cp dist/dsp.so "${TMP_DIR}/${MODULE_ID}/"
tar -czf "${ARCHIVE}" -C "${TMP_DIR}" "${MODULE_ID}"
rm -rf "${TMP_DIR}"

echo "Done: ${ARCHIVE}"
echo ""
echo "Install with ./install.sh, or upload via Schwung Manager (http://move.local:7700)."
