"""Fail closed unless the mounted project volume owns Docker's build storage."""

import json
import os
from pathlib import Path
import subprocess
import sys

DOCKER_HOST = 'unix:///run/noventra-docker.sock'


def check_storage(root):
    volume = root.resolve() / 'build'
    if not os.path.ismount(volume):
        raise RuntimeError(f'Build volume is not mounted: {volume}')
    filesystem = subprocess.check_output(
        ['findmnt', '-nr', '-M', str(volume), '-o', 'FSTYPE'], text=True).strip()
    if filesystem != 'ext4':
        raise RuntimeError(f'Build volume must use ext4, found {filesystem!r}')
    info = json.loads(subprocess.check_output(
        ['docker', '--host', DOCKER_HOST, 'info', '--format', '{{json .}}'],
        text=True, timeout=20))
    expected = volume / 'docker'
    actual = info.get('DockerRootDir')
    if not actual or Path(actual).resolve() != expected:
        raise RuntimeError(f'Docker storage must be {expected}, found {actual!r}')


if __name__ == '__main__':
    try:
        check_storage(Path(__file__).resolve().parents[2])
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
        sys.exit(f'Build storage check failed: {error}')
    print('Verified: ext4 build volume and dedicated Docker storage.')
