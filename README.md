# Noventra Browser

Noventra is an open source desktop browser built on Helium and Chromium, with
vertical tabs, Spaces, split views and configurable browser controls.

[Downloads and signed updates](https://downloads.noventrastudio.com/) ·
[GitHub releases](https://github.com/alexsimeonoww/noventra-browser/releases)

The first public target is **Linux x86_64 preview**. Windows and macOS downloads
are not available yet. Preview builds need testing before everyday use.

## Noventra 0.1.8: Settings and protected streaming setup

Settings cards keep their controls visible in narrow windows, and interrupted
Spell check expansion no longer clips its contents. Tab-memory previews are
visible under Customize Noventra, Tabs & browsing, Tab previews. Linux
default-browser selection handles Omarchy's dispatcher, and the installer
registers simple launcher paths so Omarchy can discover them.

The protected-streaming controls explain optional Widevine installation and
when to quit completely and reopen. Public encrypted playback passed. Netflix
and Voyo account playback remain unverified. See the
[settings and streaming guide](docs/noventra/settings-media-default-browser-2026-11-05.md).

## Noventra 0.1.7: separate Space profiles and compact pins

Regular Linux Spaces use independent profiles for history, bookmarks, saved
passwords, download lists, extensions and settings. Switching Spaces retains
their loaded pages in one window. Existing shared library data stays in
Personal; each other Space keeps its website partition through a cold migration
with a retained recovery copy. Incognito belongs to the Space that opened it.
Ordinary private visits are not saved, while suggestions and explicit bookmark
saves use that parent Space.

Pins use smaller tiles, keep their favicon colors while closed, and show Close
only while loaded. Pinned and ordinary tabs can join a split in the same Space;
the resulting split stays pinned. See [profile and migration details](docs/noventra/profile-spaces.md).
Version 0.1.7 is delivered through the existing signed update feed.

## Visible leave confirmations

Linux preview 6 fixes an invisible leave confirmation on Wayland. A misplaced
prompt could dim the browser and block tab switching when closing sites such
as Gmail. Cancel and Leave now remain visible. Cancel or Escape preserves the
page and its draft; Leave closes the intended tab.

The corrected binary passed 116 native checks across both tab layouts on
private Wayland and X11 displays. The tests use local pages, not a signed-in
mail account. Native Mac, Windows and full Chromium suites were not run.

## Sidebar space controls and floating resize

Linux preview 5 fixes the hidden third space in the default expanded sidebar
when Bookmarks and More share its footer. Space controls use the available width
without counting the footer's padding twice. It also fixes an animation
completion problem that could crash the browser while a floating sidebar was
being narrowed. See [the follow-up notes](docs/noventra/sidebar-space-capacity-animation-2026-11-04.md)
for the verification scope.

## Closing pinned sites

Linux preview 4 keeps a pinned site's shortcut when its page is closed. Hover
or focus a pin to reveal its small X, or press Ctrl+W to unload the page. Click
the pin to load it again. Its name, saved home and account stay attached to the
shortcut; Unpin turns it into an ordinary tab. Normal leave confirmations apply,
and closing one pinned split pane preserves its partner. See
[pin closing and Incognito](docs/noventra/pin-close-incognito-spaces-2026-11-04.md).

## Incognito Spaces

On Linux, New Incognito (`Ctrl+Shift+N`) opens a temporary Incognito Space in
the same window. Its tabs use an off-the-record profile with separate cookies
and site storage. Closing its last tab removes that Space; regular Spaces stay
available beside it. Preview 4 keeps a live private shortcut visible after
returning to Personal, including in the compact rail or a crowded sidebar.
Returning to it preserves the loaded private page and its draft.

## Updates and moving to another PC

Linux preview 3 offers **Update and restart** in **About Noventra** once a
verified update is ready. It closes the browser normally, installs the signed
update, then reopens regular tabs and Spaces. Cancelling an unsaved-page or
Incognito warning cancels the restart. Incognito sessions are not restored.

To receive the latest Linux release, open About Noventra and use **Update and
restart** once the verified update is ready. Preview 2 uses its existing flow:
let About prepare the update, then close every Noventra window and reopen it
once. The button is hidden while the installed version is current.

Setup now offers **Move from another Noventra PC** above imports from other
browsers on the current PC. It opens **Data & devices** in a separate tab for a
direct connection code or encrypted `.noventra` file, keeping setup open.
Transfer supports reviewed bookmarks, Spaces, tabs, appearance/layout, an
extension reinstall list and optional built-in saved passwords. Website
sessions and browsing history are excluded. See [device transfer](docs/noventra/device-transfer-2026-10-30.md).

## Cookie import and update reminders

Linux preview 2 adds optional cookie import for supported Chromium-family and
Firefox source formats. Select **Cookies and signed-in sessions** in setup or
Settings. The choice starts unchecked, and imported sessions belong to the
Space where the import started. Some websites still require another sign-in.
See [cookie import limits](docs/noventra/cookie-import-2026-11-03.md).

The installed launcher checks for updates while Noventra remains open. It can
send a desktop reminder after a verified update is ready, at most once every
twelve hours. It never forces a restart. Desktop reminders require `notify-send`
and a notification service. Preview 3 directs the reminder to About Noventra's
update action. Older previews gain the new behavior through their upgrade.

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

The included native regression fixture source is documented in
[the native fixture notes](tests/noventra/update_restart/README.md). Its private
display and network harness is separate from the public CI unit tests.

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
