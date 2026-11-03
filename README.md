# Noventra Browser

Noventra is an open source desktop browser built on Helium and Chromium, with
vertical tabs, Spaces, split views and configurable browser controls.

[Downloads and signed updates](https://downloads.noventrastudio.com/) ·
[GitHub releases](https://github.com/alexsimeonoww/noventra-browser/releases)

The first public target is **Linux x86_64 preview**. Windows and macOS downloads
are not available yet. Preview builds need testing before everyday use.

## Incognito Spaces

On Linux, New Incognito (`Ctrl+Shift+N`) opens a temporary Incognito Space in
the same window. Its tabs use an off-the-record profile with separate cookies
and site storage. Closing its last tab removes that Space; regular Spaces stay
available beside it.

## Cookie import and update reminders

Linux preview 2 adds optional cookie import for supported Chromium-family and
Firefox source formats. Select **Cookies and signed-in sessions** in setup or
Settings. The choice starts unchecked, and imported sessions belong to the
Space where the import started. Some websites still require another sign-in.
See [cookie import limits](docs/noventra/cookie-import-2026-11-03.md).

The installed launcher checks for updates while Noventra remains open. Preview 2
can send a desktop reminder after a verified update is ready, at most once every
twelve hours. It never forces a restart. Desktop reminders require `notify-send`
and a notification service. Preview 1 gains this feature through its upgrade;
use About Noventra to check immediately for that first update.

## Source and builds

This repository contains Noventra's complete modified prepared source overlay
in `prepared-source/`, its Linux patch series, resources, build and release
tools. `noventra/source-identity.json` inventories the exact source/build inputs
for the corresponding release. Unchanged Chromium dependencies are fetched from
the pinned upstream projects; they are not vendored here. This is not a claim
of byte-identical reproducible builds.

Build in a separate, fresh Linux workspace with the prerequisites documented by
[Helium Linux](https://github.com/imputnet/helium-linux). A Chromium build needs
hundreds of GB of storage and substantial memory. Read the exact engine pins in
`noventra/patches/chromium-156/manifest.json`, clone Helium Linux at its `platform`
revision, and initialize its pinned `helium-chromium` submodule at the `helium`
revision. Copy this repository's inventoried build inputs into that checkout,
preserving relative paths and executable modes. The release's source ZIP keeps
these inputs in an explicit `build-inputs/` directory for that purpose.

Run `scripts/build.sh -c --prepare-only`, then `scripts/build.sh --resume` in
that separate checkout. The supplied admission checks refuse to adopt or reset
an unrelated source/build tree. Preparation applies pinned Helium changes,
then the Noventra series and resources, and checks the generated GN arguments
against `noventra/qualified-args.gn`.

## Contributing and releases

Open an issue with your operating system, version and reproducible steps. Never
include passwords, browser profiles or private browsing data. Pull requests run
the small package, delivery and hosting checks on GitHub-hosted runners. Native
browser builds and privacy/UI qualification run separately; the workflow does
not compile Chromium or publish untested binaries.

[Release documentation](docs/noventra/github-releases.md) explains the source
identity, versioned GitHub assets and Cloudflare delivery. Clients verify an
Ed25519 signature and pinned public key before staging an update. The launcher
applies a verified update after all installed browser windows close. Automatic
updates require system Python 3.11+, OpenSSL and filesystem support for atomic
rename exchange. A fork must use its own signing identity and hosting.

## License and upstream credits

Noventra's inherited platform code and modifications are licensed under
[GNU GPL version 3](LICENSE). Original copyright notices are retained.
[Helium](https://github.com/imputnet/helium) and
[Chromium](https://chromium.googlesource.com/chromium/src/) dependencies retain
their own licenses; [upstream license texts](noventra/licenses/) are included,
and binary packages carry the upstream notices. See
[package credits](noventra/package/CREDITS.md).
