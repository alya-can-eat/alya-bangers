#!/bin/sh
# Build dsplayer.nds with the official BlocksDS container image.
# Needs Podman or Docker (on NixOS: virtualisation.podman.enable = true;).
#   ./build.sh          build dsplayer.nds
#   ./build.sh clean    remove build output
set -e
cd "$(dirname "$0")"
IMAGE=skylyrac/blocksds:slim-latest

if command -v podman >/dev/null 2>&1; then
    exec podman run --rm --userns=keep-id -v "$PWD":/work:Z -w /work "$IMAGE" make "$@"
elif command -v docker >/dev/null 2>&1; then
    exec docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work -w /work "$IMAGE" make "$@"
else
    echo "dsplayer: install Podman or Docker first (see README.md)." >&2
    exit 1
fi
