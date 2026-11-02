# shellcheck disable=SC2148
# Qualified Noventra sources use the explicit workspace build entrypoint.
if [ -n "${BASH_VERSION:-}" ]; then
    __noventra_dev_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd -P)"
elif [ -n "${ZSH_VERSION:-}" ]; then
    __noventra_dev_root="${0:a:h:h}"
else
    echo 'dev.sh only supports bash and zsh' >&2
    return 1 2>/dev/null || exit 1
fi

he() (
    case "${1:-help}" in
        build)
            shift
            bash "$__noventra_dev_root/scripts/docker-build.sh" --resume "$@"
            ;;
        setup|reset|pull)
            printf "Legacy '%s' is disabled: it can delete source or change checkout pins.\n" "$1" >&2
            printf 'Prepare a separate fresh workspace explicitly; existing sources are preserved:\n' >&2
            printf '  %q --workspace PATH --prepare-only\n' "$__noventra_dev_root/scripts/docker-build.sh" >&2
            return 2
            ;;
        sub|unsub|namesub|nameunsub|merge|unmerge|push|pop|resources|translate)
            printf "Legacy '%s' is disabled: it changes source outside verified preparation.\n" "$1" >&2
            return 2
            ;;
        run)
            printf 'The legacy run command targets Helium and its development profile.\n' >&2
            printf 'Use the reviewed Noventra launcher explicitly; this command launches nothing:\n' >&2
            printf '  bash %q\n' "$__noventra_dev_root/run-noventra.sh" >&2
            return 2
            ;;
        help|-h|--help)
            printf 'usage: he build --workspace PATH\n'
            printf '  build resumes only a receipt-verified prepared workspace.\n'
            printf '  preparation without compilation:\n'
            printf '    %q --workspace PATH --prepare-only\n' "$__noventra_dev_root/scripts/docker-build.sh"
            printf '  legacy setup/reset/pull and source mutation commands are disabled.\n'
            ;;
        *)
            printf 'Unknown developer command: %s (use he help)\n' "$1" >&2
            return 2
            ;;
    esac
)

# Sourcing only defines the helper; it does not initialize Quilt, source a
# toolchain, alter the prompt, select a runtime or touch a browser profile.
if ! (return 0 2>/dev/null); then
    he "$@"
fi
