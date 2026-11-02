#!/usr/bin/env bash
set -euo pipefail
project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
workspace=
resume=false
prepare_only=false
clone=false
while [ $# -gt 0 ]; do
    case "$1" in
        --workspace)
            [ $# -ge 2 ] || { echo '--workspace requires a directory' >&2; exit 2; }
            workspace="$2"; shift 2;;
        --resume) resume=true; shift;;
        --prepare-only) prepare_only=true; shift;;
        -c) clone=true; shift;;
        --pgo) shift;;
        *) printf 'Unknown build argument: %s\n' "$1" >&2; exit 2;;
    esac
done
[ -n "$workspace" ] || { echo 'An explicit --workspace is required' >&2; exit 2; }
workspace="$(python3 -B - "$project" "$workspace" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1]).resolve(strict=True)
workspace = Path(sys.argv[2]).resolve(strict=True)
volume = root / 'build'
old_source = (volume / 'src').resolve()
if (not workspace.is_dir() or workspace == volume or
        not workspace.is_relative_to(volume) or
        workspace.is_relative_to(old_source) or old_source.is_relative_to(workspace)):
    raise SystemExit('Workspace must be a separate directory inside the mounted build volume')
print(workspace)
PY
)"
ci=false
if [ -n "${CI:-}" ]; then
    ci=true
    if [ "${_prepare_only:-false}" = true ]; then
        prepare_only=true
    elif ! $prepare_only; then
        resume=true
    fi
    export _prepare_only="$prepare_only"
fi
if $resume && { $clone || $prepare_only; }; then
    echo '--resume cannot prepare or clone sources' >&2
    exit 2
fi
mode=prepare
if $resume; then mode=resume; fi
layer=(python3 -B "$project/tools/noventra/engine_layer.py")
admission="$("${layer[@]}" admit --project "$project" --workspace "$workspace" --mode "$mode")"
python3 -B "$project/tools/noventra/build_storage.py"
# The lock is created only after manifest, workspace and storage admission.
mkdir -p "$workspace/build"
exec 9>"$workspace/build/noventra-engine156.lock"
flock -n 9 || { echo 'This workspace is already building' >&2; exit 1; }
"${layer[@]}" admit --project "$project" --workspace "$workspace" --mode "$mode" >/dev/null
image_id="$(python3 -B -c 'import json,sys; print(json.load(sys.stdin)["builder_image_id"])' <<<"$admission")"
docker_cmd=(docker --host unix:///run/noventra-docker.sock)
actual_image="$("${docker_cmd[@]}" image inspect --format '{{.Id}}' noventra-builder:engine156-20261030)"
[ "$actual_image" = "$image_id" ] || { echo 'Builder image differs from the qualified recipe' >&2; exit 1; }
mkdir -p "$workspace/build/tmp"
container_args=(--rm --pull never --user "$(id -u):$(id -g)"
    --cap-drop ALL --security-opt no-new-privileges --cpus 10
    --memory 17g --memory-swap 25g --pids-limit 2048
    --tmpfs /tmp:rw,exec,size=2g,mode=1777
    --env PYTHON_CPU_COUNT=10 --env PYTHONDONTWRITEBYTECODE=1
    --env TMPDIR=/repo/build/tmp
    -v "$workspace:/repo"
    -v "$project/scripts:/repo/scripts:ro"
    -v "$project/.github/scripts:/repo/.github/scripts:ro"
    -v "$project/docker/build.Dockerfile:/repo/docker/build.Dockerfile:ro"
    -v "$project/downloads.ini:/repo/downloads.ini:ro"
    -v "$project/tools/noventra:/repo/tools/noventra:ro"
    -v "$project/noventra:/repo/noventra:ro"
    -w /repo)
if [ -n "${ARCH:-}" ]; then container_args+=(--env ARCH); fi
if $ci; then
    container_args+=(--env CI --env _prepare_only --env _gha_final --env _runner_environment)
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        [ -f "$GITHUB_OUTPUT" ] && [ ! -L "$GITHUB_OUTPUT" ] || exit 2
        container_args+=(--env GITHUB_OUTPUT -v "$GITHUB_OUTPUT:$GITHUB_OUTPUT")
    fi
fi
run_phase() {
    local network="$1" entrypoint="$2"
    shift 2
    "${docker_cmd[@]}" run --network "$network" "${container_args[@]}" \
        "$image_id" bash "$entrypoint" "$@"
}
clone_args=()
if $clone; then clone_args=(-c); fi
if $ci; then
    network=host
    if $resume; then network=none; fi
    run_phase "$network" /repo/.github/scripts/build.sh "${clone_args[@]}"
    if $prepare_only; then
        "${layer[@]}" admit --project "$project" --workspace "$workspace" --mode resume >/dev/null
    fi
else
    if ! $resume; then
        run_phase host /repo/scripts/build.sh --prepare-only "${clone_args[@]}"
        "${layer[@]}" admit --project "$project" --workspace "$workspace" --mode resume >/dev/null
        if $prepare_only; then exit 0; fi
    fi
    run_phase none /repo/scripts/build.sh --resume
fi
