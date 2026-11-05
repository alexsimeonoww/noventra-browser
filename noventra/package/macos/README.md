# Native Apple silicon test package

`tools/noventra/package_macos.py` prepares an Apple-identity-signed local test DMG from a genuine, gated native build. It launches bounded headless checks on staged and DMG copied-out apps using fresh incognito profiles and mock Keychain. It never installs an app, notarizes, publishes or changes security/quarantine settings. Linux can run offline regression tests. An actual package requires macOS and an existing authorized Apple-issued application signing identity.

The native build route is documented in [BUILD.md](BUILD.md). Use Python 3.11 or newer. After that build produces its app, generated `Noventra Packaging` directory and build receipt, run on macOS:

```sh
python3 -B tools/noventra/package_macos.py \
  --app /path/to/owned-build/Noventra.app \
  --build-receipt /path/to/owned-build/build-receipt.json \
  --signing-identity "$NOVENTRA_SIGNING_IDENTITY_SHA1" \
  --output /path/to/new-unique-package-directory
```

Use a new output directory outside an application bundle and outside `/Applications`. The input app and signing inputs are read-only inputs to this operation. Failed operations retain their owned staging directory and `commands.json` for inspection; they never write a successful `package-receipt.json`. The tool detaches only the image mount it creates. Set the identity variable locally to the exact SHA-1 of an existing Apple Development, Mac Developer or Developer ID Application identity. No certificate is created or imported. A disposable executable copy must sign successfully, satisfy the Apple trust anchor and selected leaf certificate, and report a genuine TeamIdentifier before staging starts. Missing identities and private-key authorization failures stop the operation. Use the host private prompt workflow if authorization is needed; never send a password in chat.

## Direct website downloads

Noventra's distribution target is a download from `noventrastudio.com`.
Developer ID Application signing and notarization are Apple's route for apps
distributed outside the App Store. Developer ID certificates require Developer
Program membership; an Apple Development identity can support local tests but
is not the website distribution certificate. See
[Apple's direct-distribution guide](https://developer.apple.com/developer-id/).

An unsigned development preview is a separate possible delivery route. Users
can encounter Gatekeeper warnings and may need manual approval under Apple's
[unidentified-app guidance](https://support.apple.com/en-us/102445). The current
packager preserves library validation and hardened runtime and requires a
suitable identity. Its earlier protected ad-hoc DMG aborts and must not be
uploaded as an unsigned preview. An unsigned website preview needs its own
verified packaging route. No App Store listing is part of this tooling.

## Required build receipt

The native build kit must emit this format. Values below marked `SHA256` are actual lowercase 64-character hashes, not literal placeholders:

```json
{
  "format": 1,
  "kind": "noventra-macos-build",
  "target": {"os": "mac", "cpu": "arm64", "component_build": false},
  "app": {
    "name": "Noventra.app",
    "bundle_id": "local.noventra.browser",
    "tree_sha256": "SHA256"
  },
  "source": {"manifest_sha256": "SHA256"},
  "gates": {
    "source_verified": true,
    "assets_car_actool": true,
    "keychain_identity_verified": true,
    "sparkle_disabled": true
  },
  "evidence": {
    "source_verified": {"path": "evidence/source_verified.json", "sha256": "SHA256"},
    "assets_car_actool": {"path": "evidence/assets_car_actool.json", "sha256": "SHA256"},
    "keychain_identity_verified": {"path": "evidence/keychain_identity_verified.json", "sha256": "SHA256"},
    "sparkle_disabled": {"path": "evidence/sparkle_disabled.json", "sha256": "SHA256"}
  },
  "signing_inputs": {"path": "Noventra Packaging", "tree_sha256": "SHA256"}
}
```

Each evidence file is a regular JSON file relative to the receipt and contains at least:

```json
{"format": 1, "gate": "source_verified", "status": "passed"}
```

Use the matching gate name in each file and retain real native check details in it. A true gate flag alone is insufficient. Receipt paths may not traverse or follow symlink parents. These local receipts bind the exact bundle/signing inputs to the recorded checks; they are integrity records from the trusted build kit, not signed remote attestations or proof of runtime behavior.

The build kit imports `inventory_digest(Path(...))` from `package_macos.py` for both trees. Its exact contract is SHA-256 of UTF-8 `json.dumps(inventory(root), sort_keys=True, separators=(',', ':'))`. Inventory keys are relative POSIX paths. Regular files record `type: "file"`, octal `mode`, `size`, `sha256`; directories record `type: "directory"` and octal `mode`; symlinks record `type: "symlink"` and their literal `target`. Relative internal framework links are allowed; root links, broken/absolute/escaping links, special files and set-id files are rejected.

`Noventra Packaging` is the complete generated output of Chromium's `chrome/installer/mac` target, including `signing/*.py`, generated `signing/build_props_config.py`, `app-entitlements.plist`, `helper-renderer-entitlements.plist` and `helper-gpu-entitlements.plist`. Preserve its generated configuration and entitlement files. Do not substitute hand-written release signing settings.

## Validation and signing

The packager checks the actual top-level Noventra identifier/name/executable, the `local.noventra.browser` helper namespace and framework identity, compiled `Assets.car`, executable modes and ARM64 Mach-O slices. An ARM64/x86_64 universal Mach-O is accepted; x86_64-only, ELF, malformed headers, unsafe links, enabled Sparkle feeds and Sparkle bundles are rejected. Every Mach-O code part must be accounted for by the generated upstream signing plan, including Crashpad and SwiftShader libraries; unexpected or missing parts stop packaging.

The generated Chromium configuration supplies the complete code-part paths, flags, identifiers, requirements and entitlements. Every part uses the same selected identity. Signing proceeds inside out on an independent `ditto` copy. The generated app entitlements and renderer/GPU JIT entitlements are preserved. Hardened-runtime and library-validation settings stay intact. The tool rejects protection exceptions and missing generated flags. It never signs using `--deep`.

Each part must satisfy the Apple trust anchor and exact selected leaf certificate. The tool reads back its TeamIdentifier, protection flags, designated requirements and entitlements. All nested code must share the actual signing team. The final app receives strict/deep verification. These checks supplement actual runtime execution; a successful static signature check alone does not qualify an app.

The DMG contains `Noventra.app`, an `Applications` shortcut and a local-test notice. After `hdiutil verify`, the tool reopens it read-only at an owned mount point, verifies all nested signatures, copies the app into an owned directory and detaches the image. Both staged and copied-out apps must pass `--version` and a local HTML JavaScript DOM check. Only then does it emit `package-receipt.json` with `runtime_verified: true`, identity details and a hashed runtime receipt. Failed outputs retain an unqualified status. Notarization, Gatekeeper distribution acceptance, UI and real Keychain are separate qualifications.

See [RUNTIME.md](RUNTIME.md) for the actual native evidence, startup-harness diagnosis and current signing prerequisite.

## Test limits

Run offline units on Linux or macOS:

```sh
python3 -B -m unittest discover -s tests/noventra -p 'test_*macos.py' -v
```

Packaging fixtures contain header-only synthetic Mach-O data and fake asset/gate files. Runtime boundary tests launch owned synthetic programs to exercise URL handling, marker validation, timeout cleanup and isolation from unrelated processes. The units never sign or launch a real browser. Passing units does not establish that Apple-identity signing works on the target machine.

A Mac still has to build, sign, package, open and test the actual app. Gatekeeper may refuse an unnotarized download; this tooling does not remove quarantine or disable protections. Onboarding, Cocoa pickers, Safari permissions, Keychain/password import, extensions and code transfer require target-Mac checks before being claimed as working.

The onboarding offers password-manager extension installation and sign-in. It does not certify desktop-app/Touch ID integration. In particular, [1Password's additional-browser guidance](https://support.1password.com/additional-browsers/) requires an Apple-signed browser in `/Applications` for its Mac desktop connection. An ad-hoc test installer cannot promise that integration. iCloud Passwords also needs testing with Apple's supported setup on the actual Mac.

Native bundle validation repair: the pinned Chromium copy-from-disk-image `Resources/install.sh` is accepted only at the active framework resource path with SHA256 `1b541faca17a7a1a124d9e288fd27bf66785f3667d788243383ab17700489b69`. Modified or misplaced executable scripts remain rejected. The resource is never executed by the builder or packager. The current offline suite has 53 tests, including native bundle and owned-process
regressions. These tests do not compile, sign, or qualify a Mac application.
