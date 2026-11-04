"""Run native Wayland tests on an isolated, software-rendered Sway output.

Requires sway, wlroots, wayland-scanner and a C compiler. A local extracted
runtime can be selected with NOVENTRA_SWAY_RUNTIME (containing usr/bin and
usr/lib). Never connects to or sends input to the user's desktop.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) < 2:
        raise SystemExit('Usage: with_private_sway.py COMMAND [ARG ...]')
    with tempfile.TemporaryDirectory(prefix='nv-', dir='/tmp') as tmp:
        runtime = Path(tmp)
        tests = Path(__file__).resolve().parent
        protocol = tests / 'protocols/wlr-virtual-pointer-unstable-v1.xml'
        subprocess.run(['wayland-scanner', 'client-header', str(protocol), str(runtime/'virtual-pointer.h')], check=True)
        subprocess.run(['wayland-scanner', 'private-code', str(protocol), str(runtime/'virtual-pointer.c')], check=True)
        pointer = runtime/'test-pointer'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Wno-unused-parameter', str(tests/'wayland_test_pointer.c'), str(runtime/'virtual-pointer.c'), '-I', str(runtime), '-lwayland-client', '-o', str(pointer)], check=True)
        keyboard_protocol = tests/'protocols/virtual-keyboard-unstable-v1.xml'
        subprocess.run(['wayland-scanner', 'client-header', str(keyboard_protocol), str(runtime/'virtual-keyboard.h')], check=True)
        subprocess.run(['wayland-scanner', 'private-code', str(keyboard_protocol), str(runtime/'virtual-keyboard.c')], check=True)
        keyboard = runtime/'test-keyboard'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Wno-unused-parameter', str(tests/'wayland_test_keyboard.c'), str(runtime/'virtual-keyboard.c'), '-I', str(runtime), '-lwayland-client', '-lxkbcommon', '-o', str(keyboard)], check=True)
        config = runtime/'sway.conf'
        config.write_text('''output * resolution 1200x760 position 0 0 scale 1
default_border none
default_floating_border none
gaps inner 0
gaps outer 0
focus_follows_mouse no
xwayland disable
for_window [app_id="noventra-browser"] floating enable, resize set 1200 760, move position 0 0
''')
        env = os.environ.copy()
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'HYPRLAND_INSTANCE_SIGNATURE', 'SWAYSOCK'):
            env.pop(name, None)
        env.update(XDG_RUNTIME_DIR=tmp, WLR_BACKENDS='headless', WLR_RENDERER='pixman',
                   WLR_HEADLESS_OUTPUTS='1', NOVENTRA_PRIVATE_WAYLAND='1',
                   NOVENTRA_TEST_COMPOSITOR='sway', NOVENTRA_TEST_POINTER=str(pointer),
                   NOVENTRA_TEST_KEYBOARD=str(keyboard))
        if local := env.get('NOVENTRA_SWAY_RUNTIME'):
            local = Path(local).resolve()
            env['PATH'] = str(local/'usr/bin') + ':' + env['PATH']
            env['LD_LIBRARY_PATH'] = str(local/'usr/lib') + ':' + env.get('LD_LIBRARY_PATH', '')
        with (runtime/'compositor.log').open('w') as log:
            compositor = subprocess.Popen(['sway', '-c', str(config)], env=env,
                                          stdout=log, stderr=log, start_new_session=True)
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    sockets = [p for p in runtime.glob('wayland-*') if p.is_socket()]
                    ipc = list(runtime.glob('sway-ipc.*.sock'))
                    if sockets and ipc:
                        break
                    if compositor.poll() is not None:
                        raise RuntimeError((runtime/'compositor.log').read_text()[-4000:])
                    time.sleep(.1)
                else:
                    raise RuntimeError('Private compositor sockets did not appear')
                env.update(WAYLAND_DISPLAY=sockets[0].name, SWAYSOCK=str(ipc[0]))
                outputs = json.loads(subprocess.check_output(['swaymsg', '-t', 'get_outputs', '-r'], env=env, text=True))
                assert len(outputs) == 1 and outputs[0]['rect']['width'] == 1200, outputs
                print('Private software-rendered Wayland compositor ready', flush=True)
                return subprocess.run(sys.argv[1:], env=env).returncode
            finally:
                if compositor.poll() is None:
                    os.killpg(compositor.pid, signal.SIGTERM)
                    try:
                        compositor.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(compositor.pid, signal.SIGKILL)
                        compositor.wait()


if __name__ == '__main__':
    sys.exit(main())
