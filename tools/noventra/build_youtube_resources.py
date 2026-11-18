"""Build the Linux YouTube hotfix resource pack from the signed 0.1.11 pack."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
BASE_PACK_SHA256 = '28f685d0665ed764509fa8d608bba4c7e341803a8c1f046404cdb400270ce9b5'
ASSETS = {
    'IDR_UBLOCK_ASSETS_UBLOCK_QUICK_FIXES_MIN_TXT': 'assets/ublock/quick-fixes.min.txt',
    'IDR_UBLOCK_JS_BACKGROUND_JS': 'js/background.js',
    'IDR_UBLOCK_JS_STORAGE_JS': 'js/storage.js',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path, help='0.1.11 runtime containing resources.pak and resources.pak.info')
    parser.add_argument('output', type=Path, help='new resources.pak path')
    parser.add_argument('--grit', required=True, type=Path, help='Chromium 156 tools/grit directory')
    args = parser.parse_args()
    sys.path.insert(0, str(args.grit))
    from grit.format import data_pack
    from grit.format.gzip_string import GzipString

    base = (args.base / 'resources.pak').read_bytes()
    if hashlib.sha256(base).hexdigest() != BASE_PACK_SHA256:
        raise ValueError('Expected the Noventra Linux 0.1.11 resource pack')
    packed = data_pack.ReadDataPackFromString(base)
    resources = packed.resources.copy()
    symbols = {name: int(resource_id) for name, resource_id, _ in
               (line.split(',', 2) for line in (args.base / 'resources.pak.info').read_text().splitlines())}
    layer = json.loads((ROOT / 'noventra/patches/chromium-156/manifest.json').read_text())

    for symbol, filename in ASSETS.items():
        path = 'third_party/ublock/' + filename
        resource_id = symbols[symbol]
        previous = gzip.decompress(resources[resource_id])
        current = (ROOT / 'prepared-source' / path).read_bytes()
        if hashlib.sha256(previous).hexdigest() != layer['pristine_files'][path]:
            raise ValueError(f'Unexpected packaged baseline for {path}')
        if hashlib.sha256(current).hexdigest() != layer['final_files'][path]:
            raise ValueError(f'Source differs from layer manifest for {path}')
        resources[resource_id] = GzipString(current)

    output = data_pack.WriteDataPackToString(resources, packed.encoding)
    reread = data_pack.ReadDataPackFromString(output)
    if reread.resources != resources or reread.encoding != packed.encoding:
        raise ValueError('Resource pack failed round-trip verification')
    with args.output.open('xb') as stream:
        stream.write(output)
    print(hashlib.sha256(output).hexdigest(), args.output)


if __name__ == '__main__':
    main()
