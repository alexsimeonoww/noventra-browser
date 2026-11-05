# Mac source synchronization with Linux 0.1.8

The Mac source includes all portable shared changes from Linux commit
`64ba7d31b53be3d14868ee35136c97d3509dd6ad`. The sixteen-patch layer declares
677 paths. All twelve previous Mac patches, their engine pins, and their
declared pristine hashes are preserved. The complete series passes reverse and
forward replay with every final hash checked.

This is source synchronization. The current combined layer has not been
compiled or tested natively on macOS. It is not an installable Mac update.

## Shared changes present

The source includes the current Settings alignment and collapse fixes, visible
tab-memory controls, optional Widevine installation guidance, compact pins,
pinned and ordinary split-tab wiring, and the setup device-transfer entry.
It also includes the shared import UI, bridge, writer, and session/profile
infrastructure, plus responsive About-page cards.

The prior Mac native toolbar resource correction, Translate behavior,
fullscreen transitions, initial-preference loader, profile location, Keychain
identity and failure handling, and AppIcon catalog remain. The Mac Appearance
page retains its existing control choices. Build/package tools retain the
native source admission checks, owned runtime probes, exact bundle-resource
allowlist, and Apple-issued signing-identity requirements.

## Remaining platform implementations

| Area | Mac state | Work needed before enabling or releasing |
| --- | --- | --- |
| Settings, tab previews, setup transfer entry, and compact pin/split source | Shared changes present | Native compile and UI checks |
| Regular-Space browser profiles | Shared registry, migration, session, and UI code present; Linux/Aura gate retained | Native Cocoa window host and migration qualification |
| Same-window Incognito | Genuine private-window fallback retained | Native host and lifecycle qualification |
| Cookie import | Shared plumbing present; Linux/DBus capability gate retained | Mac source readers, Keychain integration, access and schema checks |
| Protected streaming | Optional Widevine controls present | Native installation, restart, encrypted playback, and service support checks |
| Automatic updates | Sparkle and Chromium updater remain disabled | Qualified Mac package, approved signing/notarization, and a signed Mac delivery route |
| Installer | Build/package tools preserved | Native build and appropriate available Apple application-signing identity |

Linux cookie encryption and Aura browser hosting cannot be enabled by changing
their platform flags. The Mac UI continues to describe its actual shared-data
model while independent profiles remain disabled. Cookie capabilities remain
hidden when the backend does not support them.

The earlier eight-patch compiler app passed headless checks with mock Keychain.
Its protected ad-hoc DMG failed before startup. Those historical checks do not
qualify this newer layer. See [native findings](RUNTIME.md).

## Build without overwriting an earlier Mac checkout

Preserve the existing compiler workspace and its compiled output. For a newer
source kit at the same engine/platform pins, follow the
[incremental update instructions](BUILD.md#update-an-existing-build). The explicit
update verifies the old source and saves changed-file backups before rebinding
the workspace. Ordinary build/resume still refuses unexplained input changes.
First builds and engine/platform changes use separate preparation.
Follow [BUILD.md](BUILD.md) and [the package guide](README.md).

`noventra/macos-source-identity.json` inventories the published Mac prepared
source and build inputs separately from the Linux release identity. The source
tree under `prepared-source-macos/` is for inspection and hash verification.
Native preparation fetches the exact pinned upstream sources and checks all
declared native preimages before applying the Mac layer.

The offline tests validate source declarations, configuration, package guards,
and process ownership. They do not establish native compilation, a usable DMG,
real Keychain behavior, Safari import, physical-device transfer, Netflix, or
Voyo playback. Native Mac testing remains required before publishing a Mac app.
