# Approved Noventra branding

`approved/` contains byte-for-byte copies of the supplied light-background
favicon (black), dark-background favicon (white), and 1254×1254 transparent
original. The browser bundles these assets; there is no website dependency.

Run `bash noventra/resources/generate.sh` with Python and Pillow to regenerate.
Desktop icons are Lanczos downsamplings of `approved/noventra-symbol.png`, with
its square transparent canvas retained. No crop, backdrop, border, shadow or
new symbol is added. `generated/manifest.json` records source and output hashes.

- Linux: `generated/linux/hicolor/<size>x<size>/apps/noventra-browser.png`,
  from 16 through 1024 pixels.
- Windows: `generated/windows/noventra.ico`, containing 16, 24, 32, 48, 64, 128
  and 256 pixel images.
- macOS: `generated/macos/noventra.icns` and `Noventra.iconset/`, including
  16/32/128/256/512 point representations at 1× and 2×.

Browser UI masks use the approved favicon alpha, with black/white contrast
selected from their actual surface colors. Native browser product aliases
paint the approved raster directly; the generated `.icon` is an alpha-pixel
fallback for SVG exporters, not a traced outline. Original RGB desktop artwork is retained.

`replacements.txt` wires the icons to the prepared Chromium resource tree,
including Windows ICO, macOS ICNS and asset-catalog PNG paths. These files do
not establish successful Windows/macOS native builds or installer qualification.
