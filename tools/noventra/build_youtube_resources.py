"""Build the Linux YouTube hotfix resource pack from the signed 0.1.13 pack."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
BASE_PACK_SHA256 = '9eaa04e0a6c13bd7cfb5d964b83017b938b7268805b0db6a7d827741b2dfb038'
BASE_ASSETS = {
    'IDR_UBLOCK_ASSETS_UBLOCK_QUICK_FIXES_MIN_TXT': (
        'assets/ublock/quick-fixes.min.txt', 'd02b509ccd42a4886b6810698cf6be6a701de491967074a44bac9b359464d34d'),
    'IDR_UBLOCK_JS_BACKGROUND_JS': (
        'js/background.js', '6f8b46335ee6966d61aa0f8d117f48c9b4b226f8f13676c071a32cab0f386c6f'),
    'IDR_UBLOCK_JS_STORAGE_JS': (
        'js/storage.js', 'a200cb5c10cdb3a109e38d09a5c82483280584ae438548396de2b66a20d5c05d'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path, help='0.1.13 runtime containing resources.pak and resources.pak.info')
    parser.add_argument('output', type=Path, help='new resources.pak path')
    parser.add_argument('--grit', required=True, type=Path, help='Chromium 156 tools/grit directory')
    args = parser.parse_args()
    sys.path.insert(0, str(args.grit))
    from grit.format import data_pack
    from grit.format.gzip_string import GzipString

    base = (args.base / 'resources.pak').read_bytes()
    if hashlib.sha256(base).hexdigest() != BASE_PACK_SHA256:
        raise ValueError('Expected the Noventra Linux 0.1.13 resource pack')
    packed = data_pack.ReadDataPackFromString(base)
    resources = packed.resources.copy()
    symbols = {name: int(resource_id) for name, resource_id, _ in
               (line.split(',', 2) for line in (args.base / 'resources.pak.info').read_text().splitlines())}
    layer = json.loads((ROOT / 'noventra/patches/chromium-156/manifest.json').read_text())

    for symbol, (filename, previous_sha256) in BASE_ASSETS.items():
        path = 'third_party/ublock/' + filename
        resource_id = symbols[symbol]
        previous = gzip.decompress(resources[resource_id])
        current = (ROOT / 'prepared-source' / path).read_bytes()
        if hashlib.sha256(previous).hexdigest() != previous_sha256:
            raise ValueError(f'Unexpected packaged baseline for {path}')
        if hashlib.sha256(current).hexdigest() != layer['final_files'][path]:
            raise ValueError(f'Source differs from layer manifest for {path}')
        resources[resource_id] = GzipString(current)

    changed = {rid for rid in resources if resources[rid] != packed.resources[rid]}
    if changed != {symbols[name] for name in BASE_ASSETS} or len(changed) != len(BASE_ASSETS):
        raise ValueError('Expected exactly the three YouTube hotfix resources to change')
    output = data_pack.WriteDataPackToString(resources, packed.encoding)
    reread = data_pack.ReadDataPackFromString(output)
    if reread.resources != resources or reread.encoding != packed.encoding:
        raise ValueError('Resource pack failed round-trip verification')
    with args.output.open('xb') as stream:
        stream.write(output)
    print(hashlib.sha256(output).hexdigest(), args.output)


if __name__ == '__main__':
    main()
