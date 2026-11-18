#!/bin/sh
# Build alya-bangers.nds with the official BlocksDS container image.
# Needs Podman or Docker (on NixOS: virtualisation.podman.enable = true;).
#   ./build.sh          build alya-bangers.nds
#   ./build.sh clean    remove build output
set -e
cd "$(dirname "$0")"
IMAGE=skylyrac/blocksds:slim-latest

if command -v podman >/dev/null 2>&1; then
    # Rootless Podman: keep-id makes the container user you.
    exec podman run --rm --userns=keep-id -v "$PWD":/work:Z -w /work "$IMAGE" make "$@"
elif command -v docker >/dev/null 2>&1; then
    if docker info --format '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
        # Rootless Docker: root inside the container already is you on the host.
        # (A different --user would map to a helper uid that can't write here.)
        exec docker run --rm -v "$PWD":/work -w /work "$IMAGE" make "$@"
    else
        # Rootful Docker: run as you, so the build files aren't owned by root.
        exec docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work -w /work "$IMAGE" make "$@"
    fi
else
    echo "alya-bangers: install Podman or Docker first (see README.md)." >&2
    exit 1
fi
