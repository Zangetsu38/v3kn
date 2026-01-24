#!/bin/bash

set -Eeuo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
LOG="$PWD/v3kn-update.log"
BIN="v3kn.bin"
SCRIPT="update-v3kn.sh"
SCREEN_NAME="v3kn"
TMPDIR=""

log() {
    printf '%s\n' "$*" | tee -a "$LOG"
}

run() {
    "$@" 2>&1 | tee -a "$LOG"
}

fail() {
    log "Error: $*"
    exit 1
}

trap 'status=$?; log "Update failed at line $LINENO (exit $status)."; exit "$status"' ERR
trap 'if [[ -n "$TMPDIR" ]]; then rm -rf -- "$TMPDIR"; fi' EXIT

MODE="screen"
if [[ $# -eq 1 && "$1" == --handover ]]; then
    MODE="handover"
elif [[ $# -ne 0 ]]; then
    fail "Usage: $SCRIPT [--handover]"
fi

log "=== v3kn auto-updater ==="
log "$(date -u)"

case "$(uname -m)" in
    aarch64|arm64) ASSET="ubuntu-aarch64-latest.zip" ;;
    x86_64|amd64) ASSET="ubuntu-latest.zip" ;;
    *) fail "Unsupported architecture: $(uname -m)" ;;
esac
URL="https://github.com/Zangetsu38/v3kn/releases/download/continuous/$ASSET"

for tool in wget unzip mktemp chmod mv rm tee; do
    command -v "$tool" >/dev/null || fail "Required tool not found: $tool"
done
if [[ "$MODE" == screen ]]; then
    command -v screen >/dev/null || fail "Required tool not found: screen"
fi

TMPDIR=$(mktemp -d "$PWD/.v3kn-update.XXXXXX")

log "[1] Downloading $ASSET..."
run wget --timeout=30 --tries=3 -O "$TMPDIR/release.zip" "$URL"

log "[2] Extracting release..."
run unzip -o "$TMPDIR/release.zip" -d "$TMPDIR/release"
for file in "$BIN" "$SCRIPT" favicon.ico; do
    [[ -s "$TMPDIR/release/$file" ]] || fail "Required file missing or empty in archive: $file"
done
run bash -n "$TMPDIR/release/$SCRIPT"
run chmod +x "$TMPDIR/release/$BIN" "$TMPDIR/release/$SCRIPT"

log "[3] Stopping old screen sessions..."
if [[ "$MODE" == screen ]]; then
    if sessions=$(LC_ALL=C screen -list 2>&1); then
        :
    elif [[ "$sessions" != *"No Sockets found"* ]]; then
        fail "Cannot list screen sessions: $sessions"
    fi
    screen_sessions=()
    while read -r session rest; do
        if [[ "$session" =~ ^[0-9]+\.v3kn$ ]]; then
            if [[ "${STY:-}" == "$session" ]]; then
                fail "Run the manual updater from outside screen '$SCREEN_NAME' to avoid closing its own session."
            fi
            screen_sessions+=("$session")
        fi
    done <<< "$sessions"
    for session in "${screen_sessions[@]}"; do
        log "Stopping screen session $session..."
        run screen -S "$session" -X quit
        for ((attempt = 0; attempt < 30; ++attempt)); do
            if ! screen -S "$session" -Q windows >/dev/null 2>&1; then
                break
            fi
            sleep 1
        done
        if screen -S "$session" -Q windows >/dev/null 2>&1; then
            fail "Screen session $session did not stop."
        fi
    done
    if wget -q --no-proxy --timeout=1 --tries=1 -O /dev/null http://127.0.0.1:3000/ 2>/dev/null; then
        fail "An HTTP server still responds on port 3000. Stop any server running outside screen before updating."
    fi
else
    log "Automatic handover: retaining the existing session or service."
fi

log "[4] Installing release..."
run mv -f -- "$TMPDIR/release/favicon.ico" "./favicon.ico"
run mv -f -- "$TMPDIR/release/$SCRIPT" "./$SCRIPT"
run mv -f -- "$TMPDIR/release/$BIN" "./$BIN"

log "[5] Cleaning temporary files..."
run rm -rf -- "$TMPDIR"
TMPDIR=""
run rm -rf -- "$PWD/v3kn_tmp"
run rm -f -- "$PWD/ubuntu-aarch64-latest.zip" "$PWD/ubuntu-latest.zip"

if [[ "$MODE" == handover ]]; then
    log "[6] Restarting $BIN in the existing session or service..."
    shopt -s execfail
    exec "./$BIN"
fi

log "[6] Starting $BIN in detached screen '$SCREEN_NAME'..."
run screen -dmS "$SCREEN_NAME" "$PWD/$BIN"
screen_seen=false
for ((attempt = 0; attempt < 30; ++attempt)); do
    if screen -S "$SCREEN_NAME" -Q windows >/dev/null 2>&1; then
        screen_seen=true
        if response=$(wget -q --no-proxy --timeout=1 --tries=1 -O - http://127.0.0.1:3000/ 2>/dev/null); then
            if [[ "$response" == *"V3KN server is running"* ]]; then
                log "Update complete. Server is responding in screen '$SCREEN_NAME'. Attach with: screen -r $SCREEN_NAME"
                exit 0
            fi
        fi
    elif [[ "$screen_seen" == true ]]; then
        fail "Screen '$SCREEN_NAME' exited before the server became ready. Check v3kn.log."
    fi
    sleep 1
done
fail "Server did not respond on port 3000 within the startup wait. Check screen -r $SCREEN_NAME and v3kn.log."
