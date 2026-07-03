#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

MODULE_ID="idoom-fx"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-root}"
REMOTE_DIR="/data/UserData/schwung/modules/midi_fx/${MODULE_ID}"

if [ ! -f dist/dsp.so ]; then
    echo "dist/dsp.so not found — run ./build.sh first" >&2
    exit 1
fi

echo "Installing ${MODULE_ID} to ${MOVE_USER}@${MOVE_HOST}:${REMOTE_DIR}"

ssh "${MOVE_USER}@${MOVE_HOST}" "mkdir -p ${REMOTE_DIR}"
scp module.json "${MOVE_USER}@${MOVE_HOST}:${REMOTE_DIR}/module.json"
scp dist/dsp.so "${MOVE_USER}@${MOVE_HOST}:${REMOTE_DIR}/dsp.so"
ssh "${MOVE_USER}@${MOVE_HOST}" "chown -R ableton:users ${REMOTE_DIR}"

echo "Done. Reboot the Move to load the new DSP:"
echo "  ssh ${MOVE_USER}@${MOVE_HOST} reboot"
