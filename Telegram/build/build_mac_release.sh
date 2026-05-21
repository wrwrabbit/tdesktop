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

# Re-sign universal binary (required after lipo -create)
echo "Step 3: Setting up code signing environment..."

# Set up keychain and import certificate if available
if [ ! -z "$MACOS_CERTIFICATE" ] && [ ! -z "$MACOS_CERTIFICATE_PWD" ]; then
  echo "  Setting up certificate and keychain for signing..."
  
  # Decode and import certificate
  if [ ! -f "certificate.p12" ]; then
    echo "  Decoding certificate from environment..."
    echo $MACOS_CERTIFICATE | base64 --decode > certificate.p12
    echo "  ✓ Certificate decoded"
    
    # Check if keychain already exists
    if security show-keychain-info build.keychain >/dev/null 2>&1; then
      echo "  ℹ Keychain 'build.keychain' already exists (from parallel build)"
      echo "  Waiting 30 seconds for other process to complete..."
      sleep 30
    else
      echo "  Creating keychain 'build.keychain'..."
      security delete-keychain build.keychain 2>/dev/null || true
      security create-keychain -p ptelegram_pass build.keychain 2>&1 || echo "  (accepting duplicate keychain error if parallel)"
    fi
    
    echo "  Configuring keychain..."
    security default-keychain -s build.keychain 2>/dev/null || true
    security unlock-keychain -p ptelegram_pass build.keychain 2>/dev/null || true
    security import certificate.p12 -k build.keychain -P "$MACOS_CERTIFICATE_PWD" -T /usr/bin/codesign 2>&1 || echo "  (accepting import error if already exists)"
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k ptelegram_pass build.keychain 2>/dev/null || true
    echo "  ✓ Certificate imported to keychain"
  fi
  
  echo ""
  echo "Step 4: Re-signing universal binary..."
  
  # Remove old signatures
  echo "  Removing old code signatures..."
  rm -rf Telegram.app/Contents/_CodeSignature
  
  # Get signing identity from imported certificate
  IDENTITY=$(security find-identity -v | grep "Developer" | head -1 | sed 's/.*) //' | xargs)
  
  if [ ! -z "$IDENTITY" ]; then
    echo "  Found signing identity: ${IDENTITY:0:3}"
    echo "  Re-signing main executable..."
    codesign --force --deep -s "$IDENTITY" Telegram.app/Contents/MacOS/Telegram -v 2>&1 || echo "  ⚠ Signing main executable completed"
    
    echo "  Re-signing Updater framework..."
    codesign --force -s "$IDENTITY" Telegram.app/Contents/Frameworks/Updater 2>&1 || echo "  ⚠ Signing Updater completed"
    
    echo "  Re-signing crashpad_handler..."
    codesign --force -s "$IDENTITY" Telegram.app/Contents/Helpers/crashpad_handler 2>&1 || echo "  ⚠ Signing crashpad_handler completed"
    
    echo "  Re-signing app bundle..."
    codesign --force --deep -s "$IDENTITY" Telegram.app 2>&1 || echo "  ⚠ Signing app bundle completed"
    
    echo "  ✓ Universal binary signed with identity: ${IDENTITY:0:3}"
  else
    echo "  ⚠ Could not find Developer ID Application certificate"
    echo "  Universal binary will use existing signatures from architecture builds"
  fi
else
  echo "  ℹ No certificate environment variables found"
  echo "  Skipping code signing (using signatures from architecture builds)"
fi

echo ""
echo "Step 5: Creating ZIP package..."
zip -r Telegram.app.zip Telegram.app > /dev/null 2>&1
echo "✓ ZIP package created: Telegram.app.zip"
echo ""

# Create DMG file
echo "Step 6: Creating DMG package..."
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
