#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$repo_dir"
if [ ! -x .local/bin/quote-cards ]; then
    echo "Run ./setup.sh first to build Quote Cards." >&2
    exit 1
fi
# Tk uses X11. Also force software rendering if a library ever requests OpenGL.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
if [ "$#" -gt 0 ]; then
    exec .local/bin/quote-cards "$@"
fi
# Keep just one card window, including when a session restores applications.
exec flock --nonblock --no-fork .local/quote-cards.lock .local/bin/quote-cards
