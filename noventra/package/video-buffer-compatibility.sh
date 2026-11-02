#!/bin/sh
# Shared launch policy; sourcing this file does not change the environment.

noventra_single_nvidia_card() (
    count=0
    for card in "$1"/card*; do
        number=${card##*/card}
        case "$number" in ''|*[!0-9]*) continue ;; esac
        [ -d "$card" ] || continue
        count=$((count + 1))
        [ -r "$card/device/vendor" ] || return 1
        IFS= read -r vendor < "$card/device/vendor" || return 1
        [ "$vendor" = 0x10de ] || return 1
        driver=$(readlink "$card/device/driver") || return 1
        [ "${driver##*/}" = nvidia ] || return 1
    done
    # Do not guess which GPU a hybrid or multi-GPU machine will use.
    [ "$count" = 1 ]
)

# First argument is the DRM inventory directory; remaining arguments are passed
# unchanged to Chromium. Return one additional flag only for a known setup.
noventra_video_buffer_flag() (
    drm=$1
    shift
    [ "${NOVENTRA_VIDEO_BUFFER_WORKAROUND:-auto}" = auto ] || return 1
    [ "$(uname -s)" = Linux ] || return 1
    # Respect deliberate GPU selection, including hybrid/offload configurations.
    [ -z "${DRI_PRIME:-}${__NV_PRIME_RENDER_OFFLOAD:-}${__EGL_VENDOR_LIBRARY_FILENAMES:-}${GBM_BACKEND:-}${VK_ICD_FILENAMES:-}" ] || return 1
    # NVIDIA-only desktops commonly export this for XWayland too.
    case "${__GLX_VENDOR_LIBRARY_NAME:-}" in ''|nvidia) ;; *) return 1 ;; esac
    platform=${XDG_SESSION_TYPE:-}
    for argument do
        # Chromium also accepts one leading dash for named switches.
        case "$argument" in --*) ;; -?*) argument=-$argument ;; esac
        case "$argument" in
            --) break ;;
            --ozone-platform=*) platform=${argument#*=} ;;
            --ozone-platform|--ozone-platform-hint|--ozone-platform-hint=*) return 1 ;;
            --disable-gpu|--disable-gpu=*|--disable-gpu-compositing|--disable-gpu-compositing=*|\
            --disable-gpu-memory-buffer-video-frames|--disable-gpu-memory-buffer-video-frames=*|\
            --enable-gpu-memory-buffer-video-frames|--enable-gpu-memory-buffer-video-frames=*|\
            --use-angle|--use-angle=*|--use-gl|--use-gl=*|\
            --render-node-override|--render-node-override=*|\
            --gpu-vendor-id|--gpu-vendor-id=*|--gpu-device-id|--gpu-device-id=*|\
            --headless|--headless=*) return 1 ;;
        esac
    done
    # Matches the pinned Linux Ozone auto-selection: XDG_SESSION_TYPE=wayland,
    # unless an explicit --ozone-platform argument chooses another backend.
    [ "$platform" = wayland ] || return 1
    noventra_single_nvidia_card "$drm" || return 1
    # Avoid the NVIDIA/Wayland black-video path, retaining GPU compositing,
    # WebGL and WebGPU. https://github.com/brave/brave-browser/issues/57885
    printf '%s\n' --disable-gpu-memory-buffer-video-frames
)
