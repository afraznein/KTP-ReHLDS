#!/bin/bash
# Set KTP_NO_STAGE=1 to build WITHOUT copying into the local test tree -- staging
# overwrites an artifact whose md5 may be pinned to a reviewed build.
# KTPReHLDS Linux Build Script
# Run this on your Ubuntu server or via WSL

set -e  # Exit on error

echo "========================================"
echo "KTPReHLDS Linux Build Script"
echo "========================================"
echo "Working directory: $(pwd)"

# Check for required tools
echo "Checking for required tools..."

if ! command -v cmake &> /dev/null; then
    echo "ERROR: cmake is not installed. Install with: sudo apt-get install cmake"
    exit 1
fi

if ! command -v gcc &> /dev/null; then
    echo "ERROR: gcc is not installed. Install with: sudo apt-get install build-essential g++-multilib"
    exit 1
fi

# Check for 32-bit build support
if ! dpkg --print-foreign-architectures 2>/dev/null | grep -q i386; then
    echo "WARNING: 32-bit architecture support may not be enabled"
    echo "You may need to run: sudo dpkg --add-architecture i386 && sudo apt-get update"
fi

echo "All required tools found!"
echo ""

# Make sure scripts have unix line endings and are executable
sed -i 's/\r$//' build.sh
sed -i 's/\r$//' rehlds/version/appversion.sh
chmod +x build.sh
chmod +x rehlds/version/appversion.sh

# Success is judged on the ARTIFACTS, never on `build/` existing.
#
# build.sh does `rm -rf build; mkdir build` up front and then `exit 0`
# unconditionally (upstream ReHLDS code, at its `main $*` tail), so a failed
# `make` never reaches this script and `set -e` never fires. The old
# `if [ -d "build" ]` test therefore printed "BUILD COMPLETE!" over a compiler
# error, exited 0, and left the PREVIOUS engine_i486.so/hlds_linux in the
# staging tree — a stale binary that md5-verifies perfectly all the way to the
# fleet, because the md5 is taken from that same stale file.
# Do not "simplify" this back to a directory check.
#
# We do not patch build.sh's `exit 0`: it is upstream, and patching it costs
# merge-ability. Instead we (a) still honour its exit code should upstream ever
# fix it, and (b) require artifacts NEWER than this run — which also catches a
# stale binary left behind should build.sh's `rm -rf build` ever fail.

BUILD_STAMP="$(mktemp)"   # reference mtime: anything older than this is not ours
trap 'rm -f "$BUILD_STAMP"' EXIT

echo "Building ReHLDS..."
set +e
bash build.sh -j=$(nproc)
BUILD_RC=$?
set -e
if [ "$BUILD_RC" -ne 0 ]; then
    echo ""
    echo "========================================"
    echo "BUILD FAILED! (build.sh exit $BUILD_RC)"
    echo "========================================"
    echo "Nothing has been staged."
    exit 1
fi

# The real gate: freshly-produced artifacts.
ENGINE=$(find build -name "engine_i486.so" -newer "$BUILD_STAMP" 2>/dev/null | head -1)
HLDS=$(find build -name "hlds_linux" -newer "$BUILD_STAMP" 2>/dev/null | head -1)
if [ -z "$ENGINE" ] || [ -z "$HLDS" ]; then
    echo ""
    echo "========================================"
    echo "BUILD FAILED!"
    echo "========================================"
    for pair in "engine_i486.so:$ENGINE" "hlds_linux:$HLDS"; do
        name="${pair%%:*}"
        [ -n "${pair#*:}" ] && continue
        if find build -name "$name" 2>/dev/null | grep -q .; then
            echo "  $name exists but predates this run — the compile did not produce it."
        else
            echo "  $name was not produced."
        fi
    done
    echo ""
    echo "NOTE: build.sh exits 0 even on a failed compile, so its exit code proves"
    echo "      nothing. Refusing to stage a stale artifact. Nothing has been staged."
    exit 1
fi

echo ""
echo "========================================"
echo "BUILD COMPLETE!"
echo "========================================"
echo "Build output in: $(pwd)/build"
echo ""

# List built files
echo "Built files:"
find build -name "*.so" -o -name "hlds_linux" 2>/dev/null | head -20

# Deploy to staging folder. Overridable so this script carries no box-specific
# path as a hard requirement -- that was the stated reason it stayed untracked
# while every sibling C++ repo tracks its own build_linux.sh. Absent both, the
# staging block is skipped entirely (the `-d` test below), so a clean clone
# builds fine and simply does not stage.
#
# Stages $ENGINE/$HLDS from the freshness gate above -- never a re-`find`, which
# would happily pick up the stale artifact the gate exists to reject.
DEPLOY_DIR="${KTP_STAGING_DIR:-/mnt/n/Nein_/KTP Git Projects/KTP DoD Server/serverfiles}"
if [ -d "$DEPLOY_DIR" ]; then
    echo ""
    if [ -n "${KTP_NO_STAGE:-}" ]; then
        echo "Staging SKIPPED (KTP_NO_STAGE set)."
        echo "  Binaries left at: $ENGINE"
        echo "                    $HLDS"
    else
        echo "Deploying to staging folder..."

        if ! cp "$HLDS" "$DEPLOY_DIR/"; then
            echo "ERROR: failed to copy $HLDS into the staging tree."
            exit 1
        fi
        echo "  -> Copied hlds_linux         (md5 $(md5sum "$HLDS" | cut -d' ' -f1))"

        if ! cp "$ENGINE" "$DEPLOY_DIR/"; then
            echo "ERROR: failed to copy $ENGINE into the staging tree."
            exit 1
        fi
        echo "  -> Copied engine_i486.so     (md5 $(md5sum "$ENGINE" | cut -d' ' -f1))"

        echo ""
        # Printed only when a copy actually happened -- it used to print regardless.
        echo "Files staged at: $DEPLOY_DIR/"
    fi
else
    echo ""
    echo "Staging folder not found: $DEPLOY_DIR"
    echo "(build succeeded; nothing staged)"
fi
