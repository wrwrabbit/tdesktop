#!/bin/bash
set -e

# build_mac_release.sh - Combine architecture-specific builds into universal macOS package
#
# This script takes a folder containing architecture-specific app bundles and creates
# a universal binary, plus ZIP and DMG packages.
#
# Usage:
#   ./build_mac_release.sh /path/to/folder
#
# Input folder structure:
#   /path/to/folder/
#   ├── Telegram.arm64.app/Contents/...
#   └── Telegram.x86_64.app/Contents/...
#
# Output:
#   /path/to/folder/
#   ├── Telegram.app/               (universal binary)
#   ├── Telegram.arm64.app/         (original, preserved)
#   ├── Telegram.x86_64.app/        (original, preserved)
#   ├── Telegram.app.zip            (zipped universal app)
#   └── tsetup.latest.dmg           (DMG package)

Error() {
  echo "ERROR: $1" >&2
  exit 1
}

# Check arguments
if [ $# -ne 1 ]; then
  Error "Usage: $0 /path/to/folder"
fi

INPUT_FOLDER="$1"

# Verify input folder exists
if [ ! -d "$INPUT_FOLDER" ]; then
  Error "Input folder not found: $INPUT_FOLDER"
fi

# Verify architecture-specific apps exist
if [ ! -f "$INPUT_FOLDER/Telegram.x86_64.app/Contents/MacOS/Telegram" ]; then
  Error "x86_64 binary not found at: $INPUT_FOLDER/Telegram.x86_64.app/Contents/MacOS/Telegram"
fi

if [ ! -f "$INPUT_FOLDER/Telegram.arm64.app/Contents/MacOS/Telegram" ]; then
  Error "arm64 binary not found at: $INPUT_FOLDER/Telegram.arm64.app/Contents/MacOS/Telegram"
fi

echo ""
echo "Telegram Desktop Universal Package Builder"
echo "==========================================="
echo "Input folder: $INPUT_FOLDER"
echo ""

# Create universal binary
echo "Step 1: Creating universal binary..."
cd "$INPUT_FOLDER"

# Clean up any existing universal app
rm -rf Telegram.app
cp -R Telegram.x86_64.app Telegram.app

echo "  Combining main executable..."
lipo -create \
  Telegram.x86_64.app/Contents/MacOS/Telegram \
  Telegram.arm64.app/Contents/MacOS/Telegram \
  -output Telegram.app/Contents/MacOS/Telegram

echo "  Combining Updater framework..."
lipo -create \
  Telegram.x86_64.app/Contents/Frameworks/Updater \
  Telegram.arm64.app/Contents/Frameworks/Updater \
  -output Telegram.app/Contents/Frameworks/Updater

echo "  Combining crashpad_handler..."
lipo -create \
  Telegram.x86_64.app/Contents/Helpers/crashpad_handler \
  Telegram.arm64.app/Contents/Helpers/crashpad_handler \
  -output Telegram.app/Contents/Helpers/crashpad_handler

echo "✓ Universal binary created!"
echo ""

# Verify universal binary
echo "Step 2: Verifying universal binary..."
MAIN_ARCHS=$(lipo -info Telegram.app/Contents/MacOS/Telegram)
echo "  Main executable: $MAIN_ARCHS"

UPDATER_ARCHS=$(lipo -info Telegram.app/Contents/Frameworks/Updater)
echo "  Updater: $UPDATER_ARCHS"

HANDLER_ARCHS=$(lipo -info Telegram.app/Contents/Helpers/crashpad_handler)
echo "  crashpad_handler: $HANDLER_ARCHS"

echo "✓ Universal binary verified!"
echo ""

# Create ZIP file
echo "Step 3: Creating ZIP package..."
zip -r Telegram.app.zip Telegram.app > /dev/null 2>&1
echo "✓ ZIP package created: Telegram.app.zip"
echo ""

# Create DMG file
echo "Step 4: Creating DMG package..."
if ! command -v create-dmg &> /dev/null; then
  echo "⚠ create-dmg not found. DMG creation skipped."
  echo "   Install with: brew install create-dmg"
else
  if [ -f tsetup.latest.dmg ]; then
    rm tsetup.latest.dmg
  fi
  
  create-dmg \
    --volname "Telegram Desktop" \
    --volicon "./Telegram.app/Contents/Resources/AppIcon.icns" \
    --hide-extension "Telegram.app" \
    --icon-size 100 \
    --app-drop-link 400 20 \
    --bless \
    --format UDBZ \
    tsetup.latest.dmg \
    ./Telegram.app
  echo "✓ DMG package created: tsetup.latest.dmg"
fi

echo ""
echo "✓ Universal package build complete!"
echo ""
echo "Output files in: $INPUT_FOLDER"
echo "  - Telegram.app/              (universal app bundle)"
echo "  - Telegram.app.zip           (zipped universal app)"
echo "  - tsetup.latest.dmg          (DMG installer)"
echo ""
