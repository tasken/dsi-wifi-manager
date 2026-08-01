#!/bin/bash
set -eo pipefail

# Change to the script's directory
cd "$(dirname "$0")"

BUILD_LOG="build.log"

# Check for Docker installation.
if ! command -v docker &> /dev/null; then
    echo "Error: docker is not installed. Please install Docker."
    exit 1
fi

# Prefer rootless Docker. Some installations expose the daemon socket only to root; retain
# the old privileged path as a fallback so `./build.sh` works on both without making sudo the
# default for people already in the docker group.
DOCKER=(docker)
if ! docker info &> /dev/null; then
    if ! command -v sudo &> /dev/null; then
        echo "Error: Docker is unavailable and sudo is not installed."
        exit 1
    fi
    DOCKER=(sudo docker)
fi
if ! "${DOCKER[@]}" compose version &> /dev/null; then
    echo "Error: Docker Compose v2 is not available."
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
# The container writes through a bind mount, so match its UID/GID to the invoking shell. When
# the Docker daemon needs elevation, SUDO_UID/SUDO_GID preserve the actual user's ownership.
BUILD_UID="${SUDO_UID:-$(id -u)}"
BUILD_GID="${SUDO_GID:-$(id -g)}"
echo "Running: ${DOCKER[*]} compose run --rm --build -T --user \"$BUILD_UID:$BUILD_GID\" dsi_wifi_manager sh -c \"$CMD\" (log: $BUILD_LOG)"
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
# -T disables TTY allocation. Without it `docker compose run` gives the container a pseudo-TTY
# and writes its output straight to the terminal, so everything after "Container Created" misses
# the pipe and never reaches tee. A build.log that stops before the compiler runs looks like a
# clean build, which is worse than no log: grepping it for warnings can then only ever pass.
#
# Observed exactly that -- build.log's mtime landed two seconds before arm9.elf's, so tee had
# already closed while make was still going.
"${DOCKER[@]}" compose run --rm --build -T --user "$BUILD_UID:$BUILD_GID" dsi_wifi_manager sh -c "$CMD" 2>&1 | tee "$BUILD_LOG"
