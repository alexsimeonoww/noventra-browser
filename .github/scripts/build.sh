#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
if [ "${_prepare_only:-false}" = true ]; then
    exec bash "$root/scripts/build.sh" --prepare-only "$@"
fi
[ $# -eq 0 ] || { echo 'CI resume takes no preparation arguments' >&2; exit 2; }
set +e
timeout -k 5m -s INT 18000s bash "$root/scripts/build.sh" --resume
rc=$?
set -e
if [ "${_gha_final:-false}" != true ] && [ "$rc" -eq 124 ]; then
    echo 'status=running' >> "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
    exit 0
elif [ "$rc" -eq 0 ]; then
    test -x "$root/build/src/out/Default/noventra-browser"
    test -x "$root/build/src/out/Default/chromedriver"
    echo 'status=completed' >> "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
fi
exit "$rc"
