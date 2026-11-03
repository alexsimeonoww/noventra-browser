# Signed Linux update delivery

Noventra's public update origin is
`https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/`.
The installed package pins the release signer's Ed25519 public key. A public key
downloaded beside a manifest does not establish client trust. Forks need their
own signing identity and hosting.

The shared verifier in `tools/noventra/release_manifest.py` and
`tools/noventra/update_delivery.py` is used by the standalone manual delivery
commands and the configured desktop updater. Requirements are Linux, Python
3.11+ and system OpenSSL with Ed25519 support. There are no Python package
dependencies. Signatures authenticate delivery; the Linux AppDir has no OS
application signature.

## Authentication and downloads

The client verifies the detached signature over the exact raw `manifest.json`
bytes before accepting JSON fields. It rejects duplicate or unknown fields,
wrong product/channel/architecture/key, expired or future manifests, and
inconsistent sequence reuse or replay. Each release or manifest renewal uses a
strictly increasing sequence; version labels do not order updates. The current
public publisher limits sequences to JavaScript's safe integer range and
manifest lifetimes to at most 31 days. Public preparation defaults to 30 days;
renewal remains an explicit release operation.

The manifest binds the distribution inventory and compressed archive by size and
SHA-256. The client permits HTTPS redirects only within the configured origin and
base path. Plain HTTP is accepted only for literal loopback addresses used by
local fixtures. There is no insecure TLS option. Requests identify Noventra with
a descriptive User-Agent and use bounded sizes, redirect hops and deadlines.

Archive verification bounds compressed and expanded sizes, checks PAX metadata,
and copies only expected members. Duplicate paths, traversal, special devices,
hardlinks, sparse files, privileged modes and undeclared files are rejected.
Only the package's known relative launcher symlink is allowed. Every extracted
file's hash, size and mode must match the authenticated inventory. Downloaded
package scripts are data; the installed trusted installer performs replacement.

## Installed browser behavior

The desktop launcher checks while the browser is open and stages a verified
release. About Noventra can request a check. An update is applied on a later
launch after every installed window has closed; it never forces a browser quit.
Builds with the one-click updater expose **Update and restart** in About after
verification. Chromium saves the regular session and handles close prompts.
Its Linux restart then waits for the old native process to exit and uses the
same managed launch and atomic installer. Cancelling a close prompt cancels
the restart. Earlier previews keep their close-and-reopen flow until upgraded.
Profiles, passwords and bookmarks remain outside application replacement.

Installed app activation and rollback use Linux `renameat2(RENAME_EXCHANGE)` so
the launcher path remains present. A capability probe refuses unsupported
filesystems before changing installed files. The launcher and manual installer
share a lock through the native exec handshake. A durable transaction journal
allows normal startup to reconcile an interrupted update before launching.
Changed or unknown installed files stop replacement and remain for review.

The private update state is bound to the feed origin/path, channel,
architecture and signer. It retains the highest authentic sequence across
updates and configured uninstall/reinstall. Automatic key rotation or feed
migration is not implemented. Do not reset verification state to bypass errors.
The verifier does not defend against a malicious process with the same local
user's write access or hardware rollback of the entire state directory.

## Publication and verification

[Release hosting](../../noventra/release-site/README.md) describes immutable R2
objects and the channel pointer written last after remote hash verification.
[GitHub releases](github-releases.md) binds the same assets to their source
commit, tag and qualification receipt. Source commits run checks; they do not
publish browser binaries automatically.

The focused Python suites cover packaging, signatures, hostile manifests and
archives, HTTP redirects, replay, locks and interrupted installation. Recovery
coverage includes process signals and injected filesystem/state boundaries;
it is not hardware power-cut certification. Native privacy/UI, actual packages
and public HTTPS delivery must be qualified separately for each shipped target.
