"""Generate branding offline from the approved PNGs, without tracing the mark."""
import base64
import hashlib
import json
from pathlib import Path
import re

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
APPROVED = ROOT / 'approved'
OUT = ROOT / 'generated'
SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)


def png(image, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format='PNG')


def embedded_svg(data, width, height):
    encoded = base64.b64encode(data).decode('ascii')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
            f'height="{height}" viewBox="0 0 64 64">'
            f'<image width="64" height="64" href="data:image/png;base64,{encoded}"/>'
            '</svg>\n')


def desktop_tile(original):
    """Preserve the approved mark on a neutral tile visible on either desktop."""
    tile = Image.new('RGBA', (1024, 1024))
    ImageDraw.Draw(tile).rounded_rectangle(
        (16, 16, 1007, 1007), radius=192,
        fill=(245, 247, 251, 255), outline=(111, 119, 134, 255), width=8)
    mark = original.resize((832, 832), Image.Resampling.LANCZOS)
    tile.alpha_composite(mark, (96, 96))
    return tile


def main():
    OUT.mkdir(exist_ok=True)
    original = Image.open(APPROVED / 'noventra-symbol.png').convert('RGBA')
    if min(original.size) < max(SIZES) or original.width != original.height:
        raise ValueError('The desktop icon source must be square and at least 1024 pixels')
    # Desktop launchers cannot choose between the themed UI favicon variants.
    # Give the unchanged approved mark a permanent contrast-safe backing there.
    tile = desktop_tile(original)
    png(tile, OUT / 'app-icon-tile.png')
    icons = {n: original.resize((n, n), Image.Resampling.LANCZOS) for n in SIZES}
    for n in SIZES:
        image = tile.resize((n, n), Image.Resampling.LANCZOS)
        png(image, OUT / f'icon-{n}.png')
        png(image, OUT / f'linux/hicolor/{n}x{n}/apps/noventra-browser.png')
        # Keep the iconset limited to iconutil's conventional filenames.
        (OUT / f'macos/Noventra.iconset/icon_{n}.png').unlink(missing_ok=True)
    for n in (16, 32, 128, 256, 512):
        png(icons[n], OUT / f'macos/Noventra.iconset/icon_{n}x{n}.png')
        png(icons[n * 2], OUT / f'macos/Noventra.iconset/icon_{n}x{n}@2x.png')
    (OUT / 'windows').mkdir(exist_ok=True)
    original.save(OUT / 'windows/noventra.ico', sizes=[(n, n) for n in SIZES if n <= 256])
    original.save(OUT / 'macos/noventra.icns', format='ICNS',
                  append_images=[icons[n] for n in (32, 64, 128, 256, 512, 1024)])

    headers = ['// Generated from approved PNG bytes by noventra/resources/generate.py.',
               '// Do not trace, redraw, or edit this data.',
               '#ifndef UI_GFX_NOVENTRA_LOGO_DATA_H_',
               '#define UI_GFX_NOVENTRA_LOGO_DATA_H_',
               '#include <cstdint>', 'namespace gfx::noventra {']
    for variant, label in [('light', 'Light'), ('dark', 'Dark')]:
        source = APPROVED / f'noventra-favicon-{variant}.png'
        image = Image.open(source).convert('RGBA')
        # Favicons are copied byte-for-byte; UI representations only downsample.
        (OUT / f'logo-{variant}-64.png').write_bytes(source.read_bytes())
        for n in (16, 22, 32, 44):
            png(image.resize((n, n), Image.Resampling.LANCZOS), OUT / f'logo-{variant}-{n}.png')
        (OUT / f'logo-{variant}-256.png').unlink(missing_ok=True)
        data = source.read_bytes()
        headers.append(f'inline constexpr uint8_t k{label}64[] = {{')
        headers += ['  ' + ', '.join(str(x) for x in data[i:i + 24]) + ',' for i in range(0, len(data), 24)]
        headers.append('};')
        for suffix, n in [('', 32), ('-200', 64), ('-22', 22), ('-44', 44)]:
            png(image.resize((n, n), Image.Resampling.LANCZOS),
                OUT / f'wordmark{"-white" if variant == "dark" else ""}{suffix}.png')
        (ROOT / f'wordmark{"-white" if variant == "dark" else ""}.svg').write_text(
            embedded_svg(source.read_bytes(), 32, 32))
    png(Image.open(OUT / 'logo-light-22.png'), OUT / 'mono-22.png')
    (ROOT / 'mono.svg').write_text(embedded_svg((APPROVED / 'noventra-favicon-light.png').read_bytes(), 22, 22))
    (ROOT / 'app-icon.svg').write_text(embedded_svg((OUT / 'app-icon-tile.png').read_bytes(), 256, 256))
    headers += ['}  // namespace gfx::noventra', '#endif', '']
    (OUT / 'noventra_logo_data.h').write_text('\n'.join(headers))

    # Pixel-exact alpha fallback for consumers that export a vector to SVG.
    # Native painting uses the approved PNG directly; this is not a traced outline.
    alpha = Image.open(APPROVED / 'noventra-favicon-light.png').getchannel('A')
    commands = ['// Generated alpha pixels of the approved 64px favicon; do not redraw.',
                'CANVAS_DIMENSIONS, 24,', 'DISABLE_AA,']
    for y in range(64):
        x = 0
        while x < 64:
            value = alpha.getpixel((x, y))
            end = x + 1
            while end < 64 and alpha.getpixel((end, y)) == value:
                end += 1
            if value:
                commands += ['NEW_PATH,', 'DISABLE_AA,', f'PATH_COLOR_ALPHA, {value},',
                             f'MOVE_TO, {x}, {y},', f'LINE_TO, {end}, {y},',
                             f'LINE_TO, {end}, {y + 1},', f'LINE_TO, {x}, {y + 1},', 'CLOSE,']
            x = end
    commands = [re.sub(r'\d+', lambda m: f'{int(m[0]) * 24 / 64:.3f}f', line)
                if line.startswith(('MOVE_TO', 'LINE_TO')) else line for line in commands]
    (ROOT / 'product.icon').write_text('\n'.join(commands) + '\n')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    files = sorted(p for p in OUT.rglob('*') if p.is_file() and p.name != 'manifest.json')
    (OUT / 'manifest.json').write_text(json.dumps({
        'sources': {p.name: {'sha256': digest(p), 'size': list(Image.open(p).size)} for p in APPROVED.glob('*.png')},
        'desktop_source': 'approved/noventra-symbol.png',
        'resampling': 'Lanczos; approved glyph geometry retained without tracing or cropping',
        'desktop_presentation': 'Common/Linux app icons use a neutral rounded tile; Mac/Windows containers retain the approved transparent original',
        'files': {str(p.relative_to(OUT)): digest(p) for p in files},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
