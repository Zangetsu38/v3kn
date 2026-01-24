#!/bin/bash

echo "=== v3kn auto-updater ==="

# URL of the latest release archive
URL="https://github.com/zangetsu38/v3kn/releases/latest/download/ubuntu-aarch64-latest.zip"

# Local filenames
ZIP="ubuntu-aarch64-latest.zip"
BIN="v3kn.bin"
SCRIPT="update-v3kn.sh"
TMPDIR="v3kn_tmp"

echo "[1] Downloading latest release..."
wget -q -O "$ZIP" "$URL" || { echo "Download failed."; exit 1; }

echo "[2] Extracting into temporary directory..."
rm -rf "$TMPDIR"
mkdir "$TMPDIR"
unzip -o "$ZIP" -d "$TMPDIR" >/dev/null

# Check binary exists
if [ ! -f "$TMPDIR/$BIN" ]; then
    echo "Error: binary '$BIN' not found in the archive."
    exit 1
fi

# Check update script exists
if [ ! -f "$TMPDIR/$SCRIPT" ]; then
    echo "Error: update script '$SCRIPT' not found in the archive."
    exit 1
fi

echo "[3] Stopping old process..."
pkill -f "$BIN"

echo "[4] Installing new files..."
cp "$TMPDIR/$BIN" "./$BIN"
cp "$TMPDIR/favicon.ico" "./favicon.ico"
cp "$TMPDIR/$SCRIPT" "./$SCRIPT"

echo "[5] Cleaning temporary files..."
rm -rf "$TMPDIR"
rm -f "$ZIP"

echo "[6] Setting executable permissions..."
chmod +x "./$BIN"
chmod +x "./$SCRIPT"

echo "[7] Update complete. Starting service..."
screen -dmS v3kn ./"$BIN"
