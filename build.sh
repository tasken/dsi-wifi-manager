#!/bin/bash
set -eo pipefail

# Change to the script's directory
cd "$(dirname "$0")"

BUILD_LOG="build.log"

# Check for Docker installation
if ! command -v docker &> /dev/null; then
    echo "Error: docker is not installed. Please install Docker."
    exit 1
fi

case "$1" in
    clean)
        MSG="Cleaning DSi Wi-Fi Manager via Docker"
        CMD="make clean"
        ;;
    build|"")
        MSG="Building DSi Wi-Fi Manager via Docker"
        CMD="make clean && make"
        ;;
    *)
        echo "Usage: $0 [clean|build]"
        exit 1
        ;;
esac

echo "=== $MSG ==="
# Resolve the real invoking user's UID/GID, not the shell's current one: when this whole
# script is run as `sudo ./build.sh`, `id -u`/`id -g` at this point would already report 0:0
# (root), silently defeating --user below and leaving every build artifact root-owned. sudo
# exports SUDO_UID/SUDO_GID for exactly this case; fall back to id for a plain invocation.
BUILD_UID="${SUDO_UID:-$(id -u)}"
BUILD_GID="${SUDO_GID:-$(id -g)}"
echo "Running: sudo docker compose run --rm --build --user \"$BUILD_UID:$BUILD_GID\" dsi_wifi_manager sh -c \"$CMD\" (log: $BUILD_LOG)"
echo ""
# Remove any stale log before tee opens a fresh one. This belongs here and not in the
# Makefile's clean target: that target runs *inside* the piped command below, after tee has
# already opened this file, and unlinking a file a running process still holds open does not
# stop it writing -- the file just vanishes once the pipeline ends and tee closes its handle.
rm -f "$BUILD_LOG"
# --user matches the container to the host UID/GID. docker-compose.yml has no `user:`
# directive, so without this the container runs as the image's default (root) and everything
# it writes into the bind-mounted work directory ends up root-owned on the host, blocking any
# later non-sudo command from touching build/ or the ROM.
#
# pipefail above is what makes the exit status meaningful: without it a failed build inside
# this pipeline would return tee's status, and a broken build would look like a clean one.
sudo docker compose run --rm --build --user "$BUILD_UID:$BUILD_GID" dsi_wifi_manager sh -c "$CMD" 2>&1 | tee "$BUILD_LOG"
