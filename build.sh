#!/bin/sh
set -e

cd "$(dirname "$0")"

DOCKER_USER="-u $(id -u):$(id -g)"

do_clean() {
  echo "Cleaning build artifacts..."
  sudo docker compose run --rm $DOCKER_USER dsiwifimanager make clean
  rm -f build.log
}

do_build() {
  : > build.log
  sudo docker compose build 2>&1 | tee -a build.log
  sudo docker compose run --rm $DOCKER_USER dsiwifimanager 2>&1 | tee -a build.log
}

case "$1" in
  build)
    do_build
    ;;
  clean)
    do_clean
    ;;
  "")
    do_clean
    do_build
    ;;
  *)
    echo "Usage: ./run.sh [build|clean]"
    echo ""
    echo "  build   Build the project (writes build.log)"
    echo "  clean   Remove build artifacts and build.log"
    echo ""
    echo "  (no args) — clean, then build"
    exit 1
    ;;
esac
