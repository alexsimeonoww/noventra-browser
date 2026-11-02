#!/bin/sh
# Copyright 2025 The Helium Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
# Adapted for unsigned local Noventra distributions.
set -eu

CHROME_WRAPPER="$(readlink -f "$0")"
HERE="$(dirname "$CHROME_WRAPPER")"
export CHROME_WRAPPER
export CHROME_VERSION_EXTRA=local
export LD_LIBRARY_PATH="$HERE:$HERE/lib:$HERE/lib.target${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

. "$HERE/video-buffer-compatibility.sh"
if video_buffer_flag=$(noventra_video_buffer_flag /sys/class/drm "$@"); then
    set -- "$video_buffer_flag" "$@"
fi
if [ -f "$HERE/../../updater/config.json" ] && [ -x /usr/bin/python3 ] &&
    /usr/bin/python3 -I -S -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
    exec /usr/bin/python3 -I -B "$HERE/../../updater/desktop_updater.py" launch -- "$@"
fi
exec "$HERE/noventra-browser" "$@"
