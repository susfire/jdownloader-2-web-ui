#!/bin/sh
#
# Helper script that builds uuidgen from util-linux as a static binary.
#
# NOTE: This script is expected to be run under Alpine Linux using native tools.
#

set -e # Exit immediately if a command exits with a non-zero status.
set -u # Treat unset variables as an error.

# Define software versions.
# Use the same versions has Alpine 3.20.
UTIL_LINUX_VERSION=2.40.4

# Define software download URLs.
UTIL_LINUX_URL=https://www.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-${UTIL_LINUX_VERSION}.tar.xz

# Set default compilation flags for native build.
# Keep optimizations and static linking.
export CFLAGS="-Os -fomit-frame-pointer"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="$CFLAGS"
# Ensure static linking and stripping
export LDFLAGS="-fuse-ld=lld -Wl,--as-needed,-O1,--sort-common --static -Wl,--strip-all"

# Use native compilers (should be available via build-base)
export CC=clang
export CXX=clang++

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

function log {
    echo ">>> $*"
}

#
# Install required packages for native build.
#
NATIVE_PKGS="\
    curl \
    build-base \
    clang \
    lld \
    pkgconfig \
    linux-headers \
    g++ \
"

log "Installing required Alpine packages for native build..."
apk --no-cache add $NATIVE_PKGS

#
# Build util-linux.
#
mkdir /tmp/util-linux
log "Downloading util-linux..."
curl -# -L -f ${UTIL_LINUX_URL} | tar xJ --strip 1 -C /tmp/util-linux

log "Configuring util-linux for native build..."
(
    # Remove --build and --host for native configuration
    cd /tmp/util-linux && \
    ./configure \
        --prefix=/usr \
        --disable-shared \
        --enable-static \
        --disable-all-programs \
        --enable-uuidgen \
        --enable-libuuid
)

log "Compiling util-linux..."
make -C /tmp/util-linux -j$(nproc)

log "Installing util-linux to temporary location..."
# Install to the temporary directory for later copying
make DESTDIR=/tmp/util-linux-install -C /tmp/util-linux install

log "Verifying uuidgen..."
ls -l /tmp/util-linux-install/usr/bin/uuidgen
# Optional: Check linking type
# ldd /tmp/util-linux-install/usr/bin/uuidgen || echo "(Static binary expected)"
