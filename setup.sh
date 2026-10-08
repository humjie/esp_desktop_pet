#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$repo_dir"
make
./run.sh --check

autostart_dir="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
mkdir -p "$autostart_dir"
# Escape the executable path for the desktop entry's quoted Exec value.
exec_path=$(printf '%s' "$repo_dir/run.sh" | sed 's/\\/\\\\\\\\/g; s/"/\\\\"/g; s/`/\\\\`/g; s/\$/\\\\$/g; s/%/%%/g')
cat > "$autostart_dir/quote-cards.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Quote Cards
Comment=A small desktop quote flash card
Exec="$exec_path"
Terminal=false
StartupNotify=false
X-GNOME-Autostart-enabled=true
EOF
printf 'Autostart enabled: %s/quote-cards.desktop\n' "$autostart_dir"
