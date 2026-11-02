#!/bin/sh
# Copyright 2026 Noventra contributors
# SPDX-License-Identifier: GPL-3.0-only
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 -I "$HERE/installer.py" uninstall "$@"
