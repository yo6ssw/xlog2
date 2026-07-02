#!/bin/sh
#
# Install xlog2-syncd as a per-user systemd service (from a release tarball).
# Runs as YOU — it replicates your logbook using your shared secret — so no root
# is needed except (optionally) to enable lingering.
#
#   ./install.sh                     # install + enable + start
#   XLOG2_SYNC_SECRET=hunter2 ./install.sh   # also scaffold the secret
#
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
prefix="${PREFIX:-$HOME/.local}"
bindir="$prefix/bin"
unitdir="$HOME/.config/systemd/user"
cfg="$HOME/.config/xlog2/layout.ini"

mkdir -p "$bindir" "$unitdir" "$(dirname "$cfg")"

echo "==> installing binary to $bindir/xlog2-syncd"
install -m755 "$here/xlog2-syncd" "$bindir/xlog2-syncd"

echo "==> installing user unit to $unitdir/xlog2-syncd.service"
# Point ExecStart at the installed binary (the shipped unit assumes /usr/bin).
sed "s|^ExecStart=.*|ExecStart=$bindir/xlog2-syncd|" \
    "$here/xlog2-syncd.service" > "$unitdir/xlog2-syncd.service"

# Scaffold a minimal config if there's no GUI xlog2 on this box to provide one.
if [ ! -f "$cfg" ]; then
    secret="${XLOG2_SYNC_SECRET:-CHANGE_ME}"
    printf '[sync]\nenabled=true\nsecret=%s\n' "$secret" > "$cfg"
    chmod 600 "$cfg"
    echo "==> wrote a minimal $cfg"
    [ "$secret" = "CHANGE_ME" ] && \
        echo "    !! set [sync] secret to match your other nodes, then: systemctl --user restart xlog2-syncd"
fi

echo "==> enabling + starting the service"
systemctl --user daemon-reload
systemctl --user enable --now xlog2-syncd.service

# Make it run headless / at boot / after logout.
if loginctl enable-linger "$USER" 2>/dev/null; then
    echo "==> lingering enabled (runs without an active login)"
else
    echo "==> to run without being logged in, run once:  sudo loginctl enable-linger $USER"
fi

echo "Done. Follow it with:  journalctl --user -u xlog2-syncd -f"
