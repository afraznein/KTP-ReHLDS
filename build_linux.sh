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

# Run the build script
echo "Building ReHLDS..."
bash build.sh -j=$(nproc)

# Check if build succeeded
if [ -d "build" ]; then
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
    DEPLOY_DIR="${KTP_STAGING_DIR:-/mnt/n/Nein_/KTP Git Projects/KTP DoD Server/serverfiles}"
    if [ -d "$DEPLOY_DIR" ]; then
        echo ""
        if [ -n "${KTP_NO_STAGE:-}" ]; then
            echo "Staging SKIPPED (KTP_NO_STAGE set)."
            echo "  Binary left at: $BINARY_PATH"
        else
            echo "Deploying to staging folder..."

            # Find and deploy hlds_linux
            HLDS=$(find build -name "hlds_linux" 2>/dev/null | head -1)
            if [ -n "$HLDS" ] && [ -f "$HLDS" ]; then
                cp "$HLDS" "$DEPLOY_DIR/"
                echo "  -> Copied hlds_linux"
            fi

            # Find and deploy engine_i486.so
            ENGINE=$(find build -name "engine_i486.so" 2>/dev/null | head -1)
            if [ -n "$ENGINE" ] && [ -f "$ENGINE" ]; then
                cp "$ENGINE" "$DEPLOY_DIR/"
                echo "  -> Copied engine_i486.so"
            fi

            echo ""
            echo "Files staged at: $DEPLOY_DIR/"
        fi
    else
        echo ""
        echo "Staging folder not found: $DEPLOY_DIR"
    fi
else
    echo ""
    echo "========================================"
    echo "BUILD FAILED!"
    echo "========================================"
    echo "Check the output above for errors"
    exit 1
fi
