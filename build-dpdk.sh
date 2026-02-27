#!/bin/bash

# Build script for iperf3 with DPDK support
# Usage: ./build-dpdk.sh [--with-dpdk | --without-dpdk]

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Default to building without DPDK (for compatibility)
DPDK_FLAG=""
if [ "$1" = "--with-dpdk" ]; then
    DPDK_FLAG="--with-dpdk"
    echo "Building with DPDK support..."
elif [ "$1" = "--without-dpdk" ]; then
    DPDK_FLAG=""
    echo "Building without DPDK support..."
else
    echo "Usage: $0 [--with-dpdk | --without-dpdk]"
    echo "Default: building without DPDK for compatibility"
    DPDK_FLAG=""
fi

# Check if we need to run bootstrap
if [ ! -f "configure" ]; then
    echo "Running bootstrap.sh to generate configure script..."
    ./bootstrap.sh
fi

# Clean previous build
if [ -f "Makefile" ]; then
    echo "Cleaning previous build..."
    make clean || true
fi

# Configure
echo "Configuring build..."
if [ "$DPDK_FLAG" = "--with-dpdk" ]; then
    # Check if DPDK is available
    if ! pkg-config --exists libdpdk 2>/dev/null; then
        echo "ERROR: DPDK not found. Please install DPDK development packages."
        echo ""
        echo "On Ubuntu/Debian:"
        echo "  sudo apt-get install dpdk dpdk-dev"
        echo ""
        echo "Or install from source: https://www.dpdk.org/"
        exit 1
    fi

    DPDK_VERSION=$(pkg-config --modversion libdpdk)
    echo "Found DPDK version: $DPDK_VERSION"

    ./configure $DPDK_FLAG
else
    ./configure
fi

# Build
echo "Building iperf3..."
make -j$(nproc)

echo ""
echo "Build completed successfully!"
echo ""

if [ "$DPDK_FLAG" = "--with-dpdk" ]; then
    echo "iperf3 has been built with DPDK support."
    echo ""
    echo "To use DPDK mode, you need to:"
    echo "1. Configure hugepages (see DPDK_README.md)"
    echo "2. Bind network interface to DPDK driver"
    echo "3. Run iperf3 with --dpdk option"
    echo ""
    echo "Example:"
    echo "  sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 192.168.1.1 -- -l 0 -n 4"
    echo ""
    echo "For detailed instructions, see DPDK_README.md"
else
    echo "iperf3 has been built in standard mode (without DPDK)."
    echo "To build with DPDK support, run: ./build-dpdk.sh --with-dpdk"
fi

echo ""
echo "To install: sudo make install"
echo "To run tests: make check"
