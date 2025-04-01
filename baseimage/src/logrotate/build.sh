#!/bin/sh
#
# Helper script that builds logrotate as a static binary.
#
# NOTE: This script is expected to be run under Alpine Linux using native tools.
#

set -e # Exit immediately if a command exits with a non-zero status.
set -u # Treat unset variables as an error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Define software versions.
LOGROTATE_VERSION=3.21.0
POPT_VERSION=1.19

# Define software download URLs.
LOGROTATE_URL=https://github.com/logrotate/logrotate/releases/download/${LOGROTATE_VERSION}/logrotate-${LOGROTATE_VERSION}.tar.xz
POPT_URL=https://ftp.osuosl.org/pub/rpm/popt/releases/popt-1.x/popt-${POPT_VERSION}.tar.gz

# Set default compilation flags for native build.
export CFLAGS="-Os -fomit-frame-pointer"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="$CFLAGS"
# Ensure static linking and stripping
export LDFLAGS="-fuse-ld=lld -Wl,--as-needed --static -Wl,--strip-all"

# Use native compilers
export CC=clang
export CXX=clang++

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
    g++ \
    patch \
"

log "Installing required Alpine packages for native build..."
apk --no-cache add $NATIVE_PKGS

#
# Compile popt (static library needed by logrotate).
#

mkdir /tmp/popt
log "Downloading popt..."
curl -# -L -f ${POPT_URL} | tar -xz --strip 1 -C /tmp/popt

log "Configuring popt for native build..."
(
    # Remove --build and --host for native configuration
    cd /tmp/popt && \
        ./configure \
            --prefix=/usr \
            --disable-nls \
            --disable-shared \
            --enable-static
)

log "Compiling popt..."
make -C /tmp/popt -j$(nproc)

log "Installing popt into the build stage..."
# Install directly into the stage's filesystem so logrotate's configure can find it
make DESTDIR=/ -C /tmp/popt install

#
# Compile logrotate.
#

mkdir /tmp/logrotate
log "Downloading logrotate..."
curl -# -L -f ${LOGROTATE_URL} | tar -xJ --strip 1 -C /tmp/logrotate

log "Patching logrotate..."
# Ensure patch command is installed (added to NATIVE_PKGS)
patch -p1 -d /tmp/logrotate < "$SCRIPT_DIR"/messages-fix.patch

log "Configuring logrotate for native build..."
(

    # Remove --build and --host for native configuration
    cd /tmp/logrotate && \
        ./configure \
            --prefix=/usr
        # Explicitly link popt statically if needed, LDFLAGS should handle it
        # POPT_LIBS="-lpopt" POPT_CFLAGS="-I/usr/include" # Usually not needed if popt installed correctly
)

log "Compiling logrotate..."
make -C /tmp/logrotate -j$(nproc)

log "Installing logrotate to temporary location..."
# Install to the temporary directory for later copying
make DESTDIR=/tmp/logrotate-install -C /tmp/logrotate install

log "Verifying logrotate..."
ls -l /tmp/logrotate-install/usr/sbin/logrotate
# Optional: Check linking type
# ldd /tmp/logrotate-install/usr/sbin/logrotate || echo "(Static binary expected)"

