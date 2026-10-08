#!/usr/bin/env bash
# Validate the collection, export Markdown, and install it on the connected pet.
set -euo pipefail

DESKPET_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PORT="${DESKPET_PORT:-/dev/ttyACM0}"
CHECK_ONLY=false

usage() {
    cat <<'EOF'
Usage: ./update-quotes.sh [--port DEVICE] [--check]

After editing quotes/quotes.json, run this script to update quotes.md, build
the firmware, and flash the connected pet. An active deskpet.service is paused
only for flashing and restarted afterward, including when flashing fails.

  --port DEVICE  Serial device (default: /dev/ttyACM0, or DESKPET_PORT)
  --check        Validate JSON only; no files, service, or device are changed
  --help         Show this help

ESP-IDF defaults to ~/esp/esp-idf. Set IDF_PATH or IDF_PYTHON_ENV_PATH to
override the SDK or its Python environment.
EOF
}

while (($#)); do
    case "$1" in
        --port)
            if (($# < 2)) || [[ -z "$2" ]]; then
                echo 'Error: --port needs a device path.' >&2
                exit 2
            fi
            PORT="$2"; shift 2 ;;
        --check) CHECK_ONLY=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Prevent two runs from building or using the serial device at the same time.
if ! "$CHECK_ONLY"; then
    exec 9>"$DESKPET_DIR/.update-quotes.lock"
    flock -n 9 || { echo 'Error: another quote update is running.' >&2; exit 1; }
fi

QUOTE_ARGS=("$DESKPET_DIR/quotes/quotes.json")
if ! "$CHECK_ONLY"; then
    QUOTE_ARGS+=(--markdown "$DESKPET_DIR/quotes/quotes.md")
fi
python3 "$DESKPET_DIR/quotes/generate.py" "${QUOTE_ARGS[@]}"

if "$CHECK_ONLY"; then exit 0; fi
if [[ ! -e "$PORT" ]]; then
    echo "Error: $PORT is missing. Connect the pet, or use --port DEVICE." >&2
    exit 1
fi

DESKPET_IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
if [[ ! -f "$DESKPET_IDF_DIR/export.sh" ]]; then
    echo "Error: ESP-IDF not found at $DESKPET_IDF_DIR. Set IDF_PATH to your SDK." >&2
    exit 1
fi

# Use the installed SDK environment even if the system Python version differs.
DESKPET_PYTHON_ENV="${IDF_PYTHON_ENV_PATH:-}"
if [[ -z "$DESKPET_PYTHON_ENV" ]]; then
    DESKPET_IDF_MAJOR="$(sed -n 's/^set(IDF_VERSION_MAJOR \([0-9]*\)).*/\1/p' "$DESKPET_IDF_DIR/tools/cmake/version.cmake")"
    DESKPET_IDF_MINOR="$(sed -n 's/^set(IDF_VERSION_MINOR \([0-9]*\)).*/\1/p' "$DESKPET_IDF_DIR/tools/cmake/version.cmake")"
    shopt -s nullglob
    for candidate in "${IDF_TOOLS_PATH:-$HOME/.espressif}"/python_env/idf"$DESKPET_IDF_MAJOR.$DESKPET_IDF_MINOR"_py*_env; do
        if [[ -x "$candidate/bin/python" ]]; then DESKPET_PYTHON_ENV="$candidate"; fi
    done
    shopt -u nullglob
fi
if [[ ! -f "$DESKPET_PYTHON_ENV/bin/activate" ]]; then
    echo 'Error: ESP-IDF Python environment not found. Set IDF_PYTHON_ENV_PATH.' >&2
    exit 1
fi

# SDK activation scripts do not support Bash nounset.
set +u
source "$DESKPET_PYTHON_ENV/bin/activate"
source "$DESKPET_IDF_DIR/export.sh"
set -u
command -v idf.py >/dev/null || { echo 'Error: ESP-IDF activation failed.' >&2; exit 1; }

cd "$DESKPET_DIR/firmware"
echo 'Building firmware...'
idf.py build

RESTORE_HOST=false
SYSTEMCTL=(systemctl --no-ask-password)
cleanup() {
    local result=$?
    trap - EXIT INT TERM
    if "$RESTORE_HOST"; then
        echo 'Restoring deskpet.service...'
        if ! "${SYSTEMCTL[@]}" start deskpet.service; then
            echo 'Error: could not restart deskpet.service. Run: sudo systemctl start deskpet.service' >&2
            result=1
        fi
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if command -v systemctl >/dev/null && systemctl is-active --quiet deskpet.service; then
    RESTORE_HOST=true
    echo 'Stopping deskpet.service for flashing...'
    if ! "${SYSTEMCTL[@]}" stop deskpet.service; then
        # Some machines need administrative privileges to control the service.
        SYSTEMCTL=(sudo systemctl)
        "${SYSTEMCTL[@]}" stop deskpet.service
    fi
fi

echo "Flashing pet on $PORT..."
idf.py -p "$PORT" flash
echo 'Quotes installed on the pet.'
