#!/bin/bash

# install jemalloc with version checking and fallback to source build

set -euo pipefail

REQUIRED_VERSION="5.3.0"
JEMALLOC_URL="https://github.com/jemalloc/jemalloc/releases/download/${REQUIRED_VERSION}/jemalloc-${REQUIRED_VERSION}.tar.bz2"
EXPECTED_SHA256="2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa"

# Check available version
echo "Checking system jemalloc version..."
sudo apt-get update -qq
AVAILABLE_APT_VERSION=$(apt-cache policy libjemalloc-dev | awk '/Candidate:/{print $2;}')

if dpkg --compare-versions "$AVAILABLE_APT_VERSION" ge "$REQUIRED_VERSION"; then
    echo "Installing jemalloc ${AVAILABLE_APT_VERSION} from apt..."
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y libjemalloc-dev
    exit 0
fi

echo "System jemalloc ${AVAILABLE_APT_VERSION} is too old. Building ${REQUIRED_VERSION} from source..."

# Setup build environment
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT  # Cleanup on exit

cd "$BUILD_DIR"

# Download and verify
echo "Downloading jemalloc ${REQUIRED_VERSION}..."
wget --quiet "$JEMALLOC_URL"

echo "Verifying checksum..."
CALCULATED_SHA256=$(sha256sum "jemalloc-${REQUIRED_VERSION}.tar.bz2" | awk '{print $1}')

if [ "$EXPECTED_SHA256" != "$CALCULATED_SHA256" ]; then
    echo >&2 "ERROR: Hash verification failed!"
    echo >&2 "Expected: $EXPECTED_SHA256"
    echo >&2 "Got:      $CALCULATED_SHA256"
    exit 1
fi

# Build and install
echo "Building jemalloc..."
tar -xjf "jemalloc-${REQUIRED_VERSION}.tar.bz2"
cd "jemalloc-${REQUIRED_VERSION}"

./configure \
    --prefix=/usr \
    --enable-static \
    --disable-cxx \
    --enable-prof \
    --libdir=/usr/lib/x86_64-linux-gnu \
    CC=clang \
    CXX=clang++

make -j "$(nproc)"
sudo make install

echo "jemalloc ${REQUIRED_VERSION} successfully installed."