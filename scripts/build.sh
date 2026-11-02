#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
clone=false
resume=false
prepare_only=false
while [ $# -gt 0 ]; do
    case "$1" in
        --resume) resume=true; shift;;
        --prepare-only) prepare_only=true; shift;;
        -c) clone=true; shift;;
        --pgo) shift;; # Phase2 PGO is mandatory for this qualified recipe.
        *) printf 'Unknown build argument: %s\n' "$1" >&2; exit 2;;
    esac
done
if $resume && { $prepare_only || $clone; }; then
    echo '--resume cannot prepare or clone sources' >&2
    exit 2
fi
layer=(python3 -B "$root/tools/noventra/engine_layer.py")
mode=prepare
if $resume; then mode=resume; fi
# No mkdir, cleanup, download, stamp lookup or argument rewrite before admission.
"${layer[@]}" admit --project "$root" --workspace "$root" --mode "$mode"
. "$root/scripts/shared.sh"
setup_environment
if ! $resume; then
    fetch_sources "$clone" true
    apply_patches
    apply_domsub
    helium_substitution
    helium_apply_translations
    helium_version
    helium_resources
    "${layer[@]}" apply --project "$root" --workspace "$root"
    write_gn_args
    gn_gen
    "${layer[@]}" seal --project "$root" --workspace "$root"
fi
if ! $prepare_only; then
    # Resume runs no preparation, GN generation or GN argument rewriting.
    build -j8
fi
