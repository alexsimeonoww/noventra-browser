# Linux AppDir packaging and updates

This is the Noventra-owned packaging route for an **unsigned Linux**
AppDir, optionally archived as `tar.xz`. It consumes an explicitly selected,
already preserved (preferably compact) Noventra snapshot. It does not build,
strip, install, sign or publish the package. It can bundle an explicitly supplied
signed-update configuration; it does not fetch appimagetool or generate AppImage
update metadata. The upstream `scripts/package.sh` and `package/helium*` remain Helium
baseline tooling and are not used by this generator.

Public downloads are Linux x86_64 previews at
[downloads.noventrastudio.com](https://downloads.noventrastudio.com/).
[Noventra source on GitHub](https://github.com/alexsimeonoww/noventra-browser)
and each release's source & build kit identify the Noventra changes and pinned
upstream source. macOS and Windows downloads are not available yet.

From the project root:

```sh
python3 -B tools/noventra/package_distribution.py \
  --source /absolute/path/to/preserved-noventra-snapshot \
  --output /absolute/path/to/new-local-package \
  --version KNOWN-LOCAL-VERSION --tar
```

Replace the three explicit values. The output must not exist (including as a
dangling symlink), and its parent must already exist. Omit `--tar` for AppDir
only. `--version` is a caller-supplied label for this known snapshot, not a
claim about the executable's internal version. The tool never reads today's
mutable version tree, queries Git, or executes the supplied browser. Architecture
comes from the snapshot's literal `args.gn` `target_cpu`: `x64` → `x86_64`,
`arm64` → `aarch64`. Other architectures and non-Linux targets fail closed.

Omitting `--update-config` creates a package for manual delivery. To bundle the
online updater, add `--update-config /absolute/path/to/update-config.json` with:

```json
{
  "format": 1,
  "base_url": "https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/",
  "channel": "stable",
  "sequence": 1,
  "public_key": "/absolute/path/to/approved-release-public.pem"
}
```

`sequence` is the package's positive minimum accepted release sequence. Supply
only an independently approved public Ed25519 PEM, never a private key or a
combined key bundle. Fork/manual packages may use their own configuration;
the public Noventra publisher additionally requires the exact stable Linux
x86_64 feed and approved signer, with the package's minimum no greater than
its signed manifest sequence. Packaging alone does not sign a release or
activate a feed; see [public release delivery](../release-site/README.md).

The source must remain quiescent throughout packaging. `snapshot.json` must
declare `binary=noventra-browser` and a nonempty `files` map of relative paths
to SHA-256 hashes. The tool rejects duplicate JSON keys, traversal, symlinks
(including unlisted links), special files, privileged input modes, missing
runtime files, bad hashes, unsupported profile/other top-level payloads, and
missing or ambiguous compiled crashpad helper names. A deliberately selected
snapshot directory may itself be a symlink; its resolved contents cannot be.
Unlisted ordinary files are neither hashed nor copied. Mandatory inputs include
the standard runtime libraries/packs, `locales/en-US.pak`, `args.gn`,
`initial_preferences`, nonempty `provenance.json`, `LICENSE-Helium`,
`LICENSE-Chromium`, and `resources.pak.info`; the bundled uBlock manifest/schema
must agree with declared loose schema files. Runtime helpers, resources, other
declared locales, additional license notices, and optional declared validation
records are preserved. No user profile locations are searched; unlisted profile
payloads and declared top-level profile directories are not copied.

All copied files are independent copies, with executable/nonexecutable modes
normalized to `0755`/`0644`. The helper name embedded in the supplied executable
is retained, including `helium_crashpad_handler` for compatible older snapshots.
The generator uses the existing snapshot validation helpers in
`tools/noventra/snapshot.py`. It verifies approved generated icon hashes and
records the original artwork hash; it does not regenerate or modify artwork.

Output contains:

- `Noventra.AppDir/AppRun`: relocatable entry point.
- `Noventra.AppDir/install.sh`, `uninstall.sh`, `installer.py`, and
  `install-manifest.json`: standalone per-user installation/removal and the
  inner file inventory, carried inside the archive.
- `Noventra.AppDir/opt/noventra/`: manifest-listed runtime, verbatim snapshot
  manifest/provenance/licenses, and the separately named `noventra-wrapper`.
- `opt/noventra/extensions/README.noventra.txt`: an inventoried non-JSON marker
  in a `0755` directory, with file mode `0644`. Native startup queries this
  external-extension directory and creates it when absent. The marker preserves
  the directory through the archive without supplying an extension manifest or
  installing an extension; the native loader ignores non-JSON files.
- `Noventra.AppDir/usr/bin/noventra-browser`: a generated internal symlink to
  that wrapper; the binary and wrapper never overwrite each other.
- Matching `noventra-browser.desktop`, `noventra-browser.png`, hicolor icons,
  and `usr/share/metainfo/local.noventra.browser.metainfo.xml`.
- `usr/share/doc/noventra/`: source credits, these instructions, the generated
  icon manifest, and an inactive AppArmor reference for an `/opt` installation.
- `Noventra.AppDir/noventra-package.json`: explicit label, architecture,
  snapshot digest, helper selection, artwork digest, and unsigned status.
- With `--update-config`, `Noventra.AppDir/updater/` contains
  `desktop_updater.py`, `release_manifest.py`, `update_delivery.py`, the normalized
  `config.json`, and pinned `release-public.pem`.
- With `--tar`, `noventra-VERSION-ARCH_linux.tar.xz`, containing the AppDir.
- `distribution.json`: final completion marker and file/hash/mode inventory,
  including the archive hash and the one generated symlink's literal target.
  The marker cannot include its own digest. It is outside the archive because
  it records the finished archive's digest.

Only an output with a complete `distribution.json` is finished. Failure leaves
the newly created directory for inspection without that marker; choose a fresh
output for a retry. Input hashes are checked before staging and while copying,
and input metadata is checked again before completion. This is integrity and
copy validation, not authenticity, signing, provenance qualification, or proof
that the source snapshot is a working browser. No existing output is replaced.

After separate runtime qualification, launch the unpacked directory with:

```sh
/absolute/path/to/new-local-package/Noventra.AppDir/AppRun --incognito 'https://example.org/'
```

The wrappers use `exec`, preserve every argument (including explicit
`--user-data-dir`), and do not supply a profile argument. The browser's existing
NOVENTRA_CONFIG_HOME/XDG/default profile behavior therefore remains in control;
the development launcher's `work/test-profile` is not selected. Library lookup
includes the adjacent runtime, `lib`, and `lib.target`, followed by any existing
`LD_LIBRARY_PATH`. `CHROME_WRAPPER` identifies the real wrapper or, for AppRun,
`${APPIMAGE}` when explicitly supplied. Desktop actions keep `%U` and
`--incognito`; no speculative WM_CLASS is set. The portable desktop file is
not installed or registered by this tool.

## Install a Linux package

Extract the archive, then run:

```sh
./Noventra.AppDir/install.sh
```

Installation requires Python 3.8 or newer and runs as your normal user, without
`sudo`. The online updater separately requires Python 3.11 or newer and OpenSSL
with Ed25519 support; see below.
It checks the complete AppDir inventory, copies independent files to
`$XDG_DATA_HOME/noventra-browser/app` (normally
`~/.local/share/noventra-browser/app`), and adds Noventra to your application
menu using the approved icons. It works after moving/extracting the archive and
does not require this source checkout. It does not open a browser or change
your default browser. The installed files remain independent of the extracted
download, which can be removed after installation.

To uninstall:

```sh
~/.local/share/noventra-browser/app/uninstall.sh
```

You can also run `./Noventra.AppDir/uninstall.sh` from an extracted package.
Both entries accept `--data-home /absolute/path` to use an alternate user data
location; use the same value for install and uninstall. Browser profiles,
passwords, bookmarks, and any extra files are never removed by this installer.
The normal browser profile stays under its existing config directory.

The installer records exact file hashes, modes, and ownership identities and
backs up existing Noventra desktop/icon files before replacing them. Uninstall
restores those backups only while the installed file and backup remain
unchanged. Modified/replaced files, extra files, and changed parent links are
preserved and reported, along with any backup needed for manual recovery.
Only empty directories created by this installer are removed. Existing
shared directories remain. A partial/changed installation is left for review;
there is no broad directory cleanup.

Installing the same verified package again is harmless. To update, close every
window of the installed Noventra, extract the newer package, and run its
`install.sh`. There is no need to uninstall first. The installer checks the
existing installation, stages and verifies an independent replacement, then
updates the app at the same launch path. Profiles remain in the config directory;
the original desktop/icon backups remain available for eventual uninstall.

An update refuses a running installed browser, modified package/desktop/icon
files, extra package files, or an unmanaged installation. It does not delete
those files or force the browser to quit. Replacing an existing installation
requires Linux system and installation-filesystem support for atomic directory
exchange (`renameat2` with `RENAME_EXCHANGE`). Unsupported systems refuse the
replacement and keep the installed version; there is no non-atomic fallback.
If an update is interrupted, rerun the new package's installer to recover before
trying again. Keep the extracted new package until installation succeeds.
Completed updates do not retain an ever-growing collection of old app versions.

## Signed online updates

Configured public Linux previews check for signed updates after per-user
installation. The updater needs Python 3.11 or newer at `/usr/bin/python3`,
OpenSSL with Ed25519 support on the system path, enough free disk space for
download/verification and the replacement, and the atomic exchange support
described above. Python 3.8–3.10 can still install and launch the browser, but
cannot run its online updater. Packages without `--update-config` remain manual.

The updater checks in the background while Noventra is open. **About Noventra**
can also check for updates. Once a verified download is ready, builds with the
one-click updater offer **Update and restart** there. This uses the browser's
normal session saving and beforeunload prompts, then waits for its native
process to exit before applying the update and reopening the regular session.
Cancelling a close prompt cancels the restart. Private sessions are not restored.
Another installed browser instance prevents replacement and is never stopped.
Closing every Noventra window and opening it again also installs a ready update.
Previously published packages retain their existing close-and-reopen flow until
they receive a release containing the one-click updater.
If authentication, prerequisites or replacement checks fail,
it keeps the installed version. Its launch coordinator also reconciles retained
update transactions before opening the browser. Browser profiles stay separate
from application replacement.

Ed25519 signatures authenticate the update manifest and its exact archive and
inventory hashes. This is separate from OS application signing: the AppDir is
still unsigned. General manual/fork packaging does not itself create a public
update service. Public release qualification and publication are separate steps.

New profiles open setup automatically. An update keeps an existing profile's
completed setup state; use **Settings → Profiles → Set up Noventra** to reopen
it. Browser discovery is optional and starts when you choose **Find browsers**.
On Linux, supported Chrome, Chromium and Helium profiles can copy saved
passwords directly, unlocking their existing system credential store when
needed. Manual import leaves saving prompts unchanged and respects enforced
administrator restrictions. Unsupported profiles can use the password CSV
import. Bookmark HTML import and password
manager extensions are available separately.

The inner inventory checks integrity, not publisher authenticity. Public
download and update verification uses the separately signed release manifest.
The packaging generator creates no AppImage or system package.

## Runtime compatibility

New packages include the shared `video-buffer-compatibility.sh` launch policy.
It adds the narrow video-buffer workaround only on an unambiguous single-NVIDIA
Linux/Wayland setup, preserving GPU acceleration. Other GPUs/platforms and
explicit graphics choices remain unchanged. Set
`NOVENTRA_VIDEO_BUFFER_WORKAROUND=off` to disable this compatibility workaround.
Launching the native executable directly bypasses the wrapper policy.
Existing packages are immutable and are not updated by this source change.

Neither launcher installs AppArmor policy, requests privileges, changes kernel
or sandbox settings, or adds `--no-sandbox`. A host that denies the browser's
normal sandbox may refuse to run it. `apparmor.cfg` is an inactive reference
only; this workflow does not qualify sandbox integration or AppImage execution.
No AppImage is generated. Package catalog metadata does not assert an OS signing
identity or app-store registration. The public download and source links above
are separate from upstream services. Upstream source credits and license notices
are retained; see `CREDITS.md`.

Windows/macOS packaging, profile and credential migration, installer coexistence,
native launch, signing, and notarization are **not qualified** by this Linux
workflow. It does not establish a distributable release or repair native
installer/source branding. Those remain separately owned work.

Focused verification (offline fixtures only, no real browser launch/build):

```sh
python3 -B -m unittest discover -s tests/noventra -p test_package_distribution.py -v
python3 -B -m unittest discover -s tests/noventra -p test_package_installer.py -v
sh -n noventra/package/AppRun noventra/package/noventra-wrapper.sh noventra/package/install.sh noventra/package/uninstall.sh
desktop-file-validate noventra/package/noventra-browser.desktop
```

The tests launch a tiny argument-recording fixture, never a real browser. They
cover manifest/path/hash failures, helper selection, copied artifact inventory,
tar contents, failure completion markers, independent copies, and wrapper
arguments/environment. XML structure and desktop identity are checked locally;
this is not AppStream catalog publication or full catalog validation.
Installer fixtures use disposable homes and an argument recorder. They cover
moved archives, independent copies, repeat installs, staged updates, restoration,
tamper rejection, changed links/files/backups, profile retention, and actual GIO
desktop argument handling for paths with spaces and shell metacharacters.
They do not launch or qualify an actual browser or installer on another distro.

For signed manual delivery, a real AppRun launch followed by normal quit must
also pass the delivery client's `active-path` revalidation. An archive/inventory
check before launch alone cannot detect runtime-created paths. The marker above
keeps the expected directory present while strict checks still reject extra or
changed files. Private delivery staging retains its existing `0700` directory
permissions; no native source or runtime extension policy is changed.

The installed `appstreamcli validate --no-net --pedantic` reports
`url-homepage-missing` (warning), `releases-info-missing` (pedantic), and
`cid-maybe-not-rdns`, `content-rating-missing`, `developer-info-missing`
(information), exiting 3. These fields are still absent from the AppStream XML;
documenting the public download/source URLs here does not fill its catalog
metadata or establish a content rating. The XML parses successfully and its
local identity/desktop references are tested; the package does not claim
catalog validation.
