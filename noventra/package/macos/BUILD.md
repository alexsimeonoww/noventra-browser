# Build the Apple silicon test app

The current sixteen-patch ARM64 source compiled and linked incrementally in
the existing M5 MacBook workspace. Its compiler app passes isolated version
and JavaScript DOM checks with mock Keychain. The earlier protected ad-hoc DMG
aborts under library validation, and no suitable Apple signing identity was
available at the native audit. See [RUNTIME.md](RUNTIME.md) for exact source and
artifact identities. Native UI, imports, real Keychain and transfer remain
unqualified; no signed Mac download or automatic update was published.

The shared source includes five-step onboarding with a dedicated search-engine choice, one Data page and selected browser imports at Finish, immediate layout saving, separate normal/compact top layouts, theme/bookmark polish and vertical bookmark-bar defaults.
These flows still require native macOS testing.

The current checkout has 677 changed paths and sixteen ordered patches.
Shared source is synchronized with Linux 0.1.8, including compact pins and
splits, the setup transfer entry, import infrastructure, and Settings/media
fixes. Independent-profile Spaces and cookie capability advertising retain
their Linux platform gates. macOS still needs a native profile host and cookie
readers. See [the parity matrix](PARITY.md).
Same-window private Spaces currently use an Aura host; macOS retains the genuine
private-window fallback until a native same-window implementation is verified.
Sparkle and Chromium updater remain disabled. This kit does not enable Mac
automatic updates or produce a public signed/notarized download.

The twelve previous Mac patches remain unchanged. Four new shared deltas
were appended after them. The combined sixteen-patch source passes exact
reverse and forward replay checks. Native compilation and isolated headless
checks also cover this exact sixteen-patch manifest.

Use an Apple silicon Mac running native arm64 Python, an APFS compiler volume,
and a complete Xcode installation with Mac SDK **26.5 or newer**. The pinned
platform asks for Python 3.13+, `requests`, Pillow, `wget`, `zstd`, GNU coreutils
(`greadlink`), and readline. Install the Xcode Metal toolchain and accept the
Xcode license through Xcode before starting. The build tool checks host, tools,
SDK, Python imports, and free space early; it does not install dependencies or
change the selected Xcode, security settings, or desktop configuration.

The native M5 run required Pillow 12.0.0 built against Apple's stock zlib to
reproduce the ten generated PNG preimage hashes. Pillow 13 and its bundled
encoder produced different bytes/pixels. That project-local environment passed
all exact hashes; keep the hash gate intact if another image toolchain differs.

The default free-space floor is 180 GiB. This is a prerequisite check, not a
prediction or a guarantee of peak build size. Source archives, extraction and a
full Chromium build need substantial local space. Use a separate compiler
workspace outside this project and retain the project checkout while building.

## Check the source kit

From the Noventra checkout on Linux or macOS:

```sh
python3 -B tools/noventra/build_macos.py check-layer
```

This verifies the separate `noventra/patches/chromium-156-macos` manifest, patch
hash, exact patch scope, series hash, and engine pins. It reports the candidate
as uncompiled. It does not download source or start a build.

The pins are Chromium `156.0.8078.79` /
`a21ce95c24ce3964416f09ae1862f53f319eb4d0`, Helium core
`2f81b9cafa38a95071e20de3f9cd6b0aca6d3c65`, and Mac platform
`0c7da2ba0b61a2143adf1c6d56ccd0df6a53d0bf`. The Mac layer changes 677 paths;
its baseline was reconstructed and strictly replayed on Linux. Native exact
preimage verification remains mandatory.

## Prepare native source once

Make only the parent directory first; the build tool creates and owns the new
workspace. This step is for the first build or a change of engine/platform pins.
Keep this workspace and its compiled output for later source updates. For an
existing build, use the incremental update below.

```sh
mkdir -p "$HOME/NoventraMacBuilds"
NOVENTRA_MAC_WORKSPACE="$HOME/NoventraMacBuilds/noventra-156-arm64-test-1"

python3.13 -B tools/noventra/build_macos.py prepare \
  --workspace "$NOVENTRA_MAC_WORKSPACE" \
  --platform-root "$NOVENTRA_MAC_WORKSPACE/platform" \
  --clone-platform
```

`--clone-platform` acquires a compiler-input checkout into that absent, owned path
and selects the exact platform/core pins. It does not clone or alter the task's
Noventra checkout. If you already have an independent clean checkout at those
pins, pass its absolute path as `--platform-root` and omit `--clone-platform`.
Its tracked/untracked changes are checked; ignored existing upstream build output
is never used or modified. Chromium source, cache, output and staged resource
generation all live in the new workspace. Even generated Helium core resources
are staged outside the upstream `helium-chromium` subtree.

Preparation downloads the official Mac arm64 Chromium source archive using the
pinned platform's download/hash tooling. It applies the pinned core and Mac
platform layers, substitutions, translations, version metadata and upstream
resources. It checks Chromium VERSION/LASTCHANGE and **every one of the 677
Noventra preimages or expected absences** before any Noventra patch is applied.
Any mismatch stops with path, expected hash and actual hash in `failure.json`.
It then applies the binary-capable patch with strict Git checks and verifies all
postimages.

Native `actool` compiles Noventra's full AppIcon catalog, including 512@1x and
512@2x entries, into `Assets.car`. This replaces upstream's Helium catalog before
compilation. The kit verifies the separate Noventra profile directory, Keychain
service/account, and protection against loading data without the Keychain key.
It preserves native Mac fullscreen/traffic-light hooks.

Partial preparation is retained for diagnosis and cannot be blindly restarted.
Use a new workspace after a failed download, patch, or asset-generation phase;
the tool does not delete or reapply over partial source. Repeating `prepare` on
an owned, completed preparation only revalidates it.

## Compile the actual Noventra app

Use exactly the same workspace and platform path:

```sh
python3.13 -B tools/noventra/build_macos.py build \
  --workspace "$NOVENTRA_MAC_WORKSPACE" \
  --platform-root "$NOVENTRA_MAC_WORKSPACE/platform" \
  --jobs 2
```

The tool revalidates source, catalog and saved gate evidence, then configures
native GN/Siso and invokes the pinned depot_tools `autoninja.py` for `chrome`
and `chrome/installer/mac`. This is a non-component arm64 local test build, with
Sparkle and Chromium updater disabled, system Xcode selected, remote execution
disabled, and no special branded entitlements. The pinned core's non-PGO setting
is retained for this first local build. GN's evaluated values are checked as
well as the written arguments. Compile concurrency defaults to the lesser of
six jobs and the Mac's CPU count. Use `--jobs 2` or `--jobs 1` on a MacBook with
limited RAM; the chosen count is recorded in the build receipt.

A failed compile can be resumed by rerunning `build` against the same owned,
unchanged preparation. A workspace binds its input manifest, builder tool,
initial preferences, pins and paths. Ordinary build/resume refuses changed
bindings; use the explicit incremental update command to move an existing
workspace to a newer source layer at the same engine pins. Logs and failure/state
JSON remain in the workspace.

## Update an existing build

Keep one active compiler workspace for small browser changes. From the newer
source kit, supply the exact previous kit that created the workspace:

```sh
NOVENTRA_MAC_WORKSPACE="$HOME/NoventraMacBuilds/noventra-156-arm64-test-1"
NOVENTRA_MAC_PREVIOUS_KIT="/absolute/path/to/previous-source-kit"

python3.13 -B tools/noventra/update_macos.py \
  --workspace "$NOVENTRA_MAC_WORKSPACE" \
  --previous-kit "$NOVENTRA_MAC_PREVIOUS_KIT"

python3.13 -B tools/noventra/build_macos.py build \
  --workspace "$NOVENTRA_MAC_WORKSPACE" \
  --platform-root "$NOVENTRA_MAC_WORKSPACE/platform" \
  --jobs 2
```

Use the workspace's existing platform path if it is outside the workspace.
Keep the previous kit available until the update has completed. The update
requires the same engine/platform pins and an idle, owned compiler workspace.
Unexplained source edits or changed input hashes stop it before replacement.

The updater reverses the old patch series in a small staging tree, checks the
new pristine hashes, and applies the new layer there before replacing changed
files. It preserves unchanged file timestamps and the existing compiler output,
Siso state and link caches. Changed source and previous receipts are backed up
in the workspace's update history. A failed or interrupted update has a guarded
recovery path; follow its recorded recovery command before building.
For an interrupted transaction, run the same updater with `--workspace` and
`--rollback-update` pointing to its recorded `source-updates/<id>` directory.
Recovery refuses to overwrite subsequent edits.

The next build regenerates GN and lets the compiler rebuild affected targets
in the same directory. Header, build-flag or widely shared source changes can
still require substantial compilation. This path avoids another source download
and a duplicate full compiler workspace. This version refuses a shrinking patch
scope or changed native icon sources before updating. Engine/platform changes
require separate preparation.

Successful compilation produces:

- `src/out/Default/Noventra.app`, with the compiled Noventra executable, helpers
  and framework, native icon catalog and bundle identity.
- `src/out/Default/Noventra Packaging`, the actual generated Chromium signing
  configuration, code-part plan and entitlements.
- `build-receipt.json`, with app/signing-input inventories and hashed gate reports
  under `evidence/`.

The kit embeds `noventra/resources/initial_preferences.json` at
`Noventra.app/Contents/Resources/initial_preferences` before calculating the app
inventory. The Mac source patch explicitly supports this bundle fallback after
user/system preference overrides; copying a JSON file alone would not activate
Chromium's stock Mac loader. It does not bypass onboarding or write a user profile.

No browser is launched by the build tool. A successful build receipt records
compilation and static bundle checks; native onboarding/import and transfer
checks remain pending.

## Make the local DMG

After the actual native build succeeds, follow [the package instructions](README.md):

```sh
python3.13 -B tools/noventra/package_macos.py \
  --app "$NOVENTRA_MAC_WORKSPACE/src/out/Default/Noventra.app" \
  --build-receipt "$NOVENTRA_MAC_WORKSPACE/build-receipt.json" \
  --signing-identity "$NOVENTRA_SIGNING_IDENTITY_SHA1" \
  --output "$HOME/NoventraMacBuilds/noventra-156-arm64-test-1-package"
```

That separate tool requires an existing authorized Apple-issued application
signing identity. It makes an unnotarized local-test DMG and runs bounded
headless version/JS checks on staged and DMG copied-out apps. It never installs
the app or changes security settings. Real Safari/Chrome/Firefox imports,
Keychain permissions, extension installation, native fullscreen and direct
code-based transfer between live devices still require testing on the Mac.

## Offline checks for this kit

```sh
python3 -B -m unittest discover -s tests/noventra -p test_build_macos.py
python3 -B -m unittest discover -s tests/noventra -p 'test_*macos.py'
```

These check source identity, hash/absence gates, path safety, workspace ownership,
resume refusal and GN configuration. They do not simulate a successful native
build. The separate Mac layer leaves the existing Linux manifest and verifier
unchanged.

Primary build references: [pinned Helium Mac helper](https://github.com/imputnet/helium-macos/blob/0c7da2ba0b61a2143adf1c6d56ccd0df6a53d0bf/devutils/shared.sh),
[pinned platform requirements](https://github.com/imputnet/helium-macos/blob/0c7da2ba0b61a2143adf1c6d56ccd0df6a53d0bf/docs/building.md),
[pinned Chromium Mac requirements](https://github.com/chromium/chromium/blob/a21ce95c24ce3964416f09ae1862f53f319eb4d0/docs/mac_build_instructions.md).

Native bundle validation repair: the pinned Chromium copy-from-disk-image `Resources/install.sh` is accepted only at the active framework resource path with SHA256 `1b541faca17a7a1a124d9e288fd27bf66785f3667d788243383ab17700489b69`. Modified or misplaced executable scripts remain rejected. The resource is never executed by the builder or packager. The offline suite covers native bundle checks, owned-process regressions and
incremental source-update recovery. These tests do not compile, sign, or qualify a Mac application.
