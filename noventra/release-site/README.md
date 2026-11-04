# Noventra downloads and signed updates

The download page uses the main Studio site's Open Horizon design: an always-light
Geist layout, approved logo, mountain hero and black pill actions. Its font and
image resources come from `https://www.noventrastudio.com/assets/`. Only HTML CSP
allows that stylesheet/font/image origin; scripts remain disabled and signed-feed
and artifact responses retain their existing policy. Keep the actual release
catalog, checksums and unavailable-platform state independent of presentation.

The 2026-11-04 OS-aware page update is live in download Worker version
`2506d1a5-6ee3-4ac8-9e24-a0829766abfb`; the previous version
`6d8a8885-0b40-4116-90da-87b90489f6e1` is retained for rollback. It changes page
rendering without writing to R2 or publishing a browser release. This source kit accompanies
Linux preview 5 / signed sequence 5. Page presentation changes remain separate
from browser release publication.

HTML uses existing browser platform hints or a bounded User-Agent fallback to
choose guidance. Compatible Linux visitors receive the Linux download action;
other systems and known unsupported Linux CPUs see availability guidance and
an explicit manual Linux link in the platform table. Unknown visitors can
choose manually. Detection is advisory and cannot establish hardware compatibility;
Mac User-Agent strings do not reliably identify Apple silicon. The page requests
no additional client hints, uses no detection script and performs no automatic
download or redirect. Only HTML varies by these headers; signed payloads and
artifact transport are unchanged.

This Worker serves `https://downloads.noventrastudio.com/` from the dedicated
`noventra-browser-releases` R2 bucket. The existing Noventra Studio Worker, site,
mail and unrelated buckets are not part of this configuration. The public
Worker accepts **GET and HEAD only** and has no upload, listing or admin route.
The Linux feed is:

```text
https://downloads.noventrastudio.com/browser/updates/stable/linux-x86_64/
```

Only qualified Linux x86_64 previews are offered. macOS and Windows stay marked
unavailable until genuine packages are built and tested. The source download is
a **source & build kit**: exact Noventra changes, pinned upstream sources,
inventories and build instructions. It is not a bundled Chromium source tree or
a reproducible-build guarantee. Signed delivery is separate from OS application
code signing; the Linux AppDir remains unsigned.

The public source repository is
[alexsimeonoww/noventra-browser](https://github.com/alexsimeonoww/noventra-browser).
The release record and source kit bind the published build to an exact source
commit; ordinary code commits do not automatically ship browser updates.

## Linux installation and update requirements

Per-user installation runs without sudo and requires Python 3.8 or newer.
Signed online updates require Python 3.11 or newer at `/usr/bin/python3` and
OpenSSL with Ed25519 support on the system path. A configured preview checks
after per-user installation, downloads verified updates in the background,
and installs a ready update on the next launch after all Noventra windows close.
The user closes those windows; the updater never forces the browser to quit.
Packages without an update configuration retain manual delivery behavior.

Replacing an installed app requires sufficient free disk space and Linux system
and installation-filesystem support for atomic directory exchange. Unsupported
systems retain the current installation; no non-atomic replacement is attempted.
Python 3.8–3.10 remains sufficient for manual installation and browser launch,
but not the online updater. See the [packaged instructions](../package/README.md).

## Storage and publication

The small channel pointer is `browser/channels/stable/linux-x86_64.json`.
Each release's metadata lives under
`browser/releases/stable/linux-x86_64/<20-digit-sequence>-<manifest-sha256>/`.
Archives and source kits use content-addressed keys under `browser/artifacts/`
and `browser/sources/`. Immutable filename indexes preserve old updater archive
URLs even after the current channel advances. Reusing a filename for different
bytes is refused.

`tools/noventra/publish_release.py prepare` reuses the existing signed manifest
publisher and verifies the actual AppDir archive before creating a local
publication marker. Public packages must include all five updater files, use
the exact stable Linux x86_64 feed above, and pin a public-only Ed25519 key equal
to the manifest signer. The packaged minimum sequence cannot exceed the signed
sequence. Upload and GitHub mirroring repeat these checks on the validated
archive; package scripts are never executed. General manual/fork packaging
continues to support optional or differently configured updates.

Preparation also requires an independently prepared source kit and matching
`source-info.json`:

```json
{
  "format": 1,
  "product": "local.noventra.browser",
  "version_label": "THE_EXACT_PACKAGE_VERSION",
  "snapshot_sha256": "THE_EXACT_BINARY_SNAPSHOT_MANIFEST_HASH",
  "source_archive": {
    "filename": "noventra-THE_EXACT_PACKAGE_VERSION-source.zip",
    "size": 12345,
    "sha256": "THE_ACTUAL_SOURCE_ARCHIVE_SHA256"
  }
}
```

The source kit must contain the exact prepared source overlay/patches, upstream
pins and URLs, compiler arguments, required build/package tools and inventories.
The publisher verifies its declared bytes and snapshot identity; it cannot prove
that a publisher supplied every required source input or reproduce the build.

```sh
python3 -B tools/noventra/publish_release.py prepare \
  --distribution /absolute/qualified-distribution \
  --output /absolute/new-release-directory \
  --private-key /absolute/private-signing-key-outside-source.pem \
  --source-archive /absolute/noventra-VERSION-source.zip \
  --source-info /absolute/source-info.json \
  --sequence 1
```

Use a fresh output path. Neither tool creates a signing identity nor copies its
private key into the release. The default manifest lifetime is 30 days, bounded
by the delivery protocol's 31-day maximum. Renewals must increase the sequence,
even when reusing the same binary/source bytes. The publisher does not schedule
renewals; keep releases current through the release process.

## Upload without a Cloudflare token

The separate `upload-worker.mjs` is a **temporary bridge**, not the public site.
Deploy it on a temporary workers.dev hostname with a `RELEASES` R2 binding and
`UPLOAD_PUBLIC_KEY_SPKI` containing the release signer's public Ed25519 SPKI DER
as base64. `upload-wrangler.jsonc` contains only that public key. Do not attach
this temporary Worker to the public downloads domain. Remove it after upload
and verification, including when the release operation fails.

```sh
python3 -B tools/noventra/publish_release.py upload \
  --release /absolute/prepared-release-directory \
  --public-key /absolute/pinned-release-public.pem \
  --private-key /absolute/private-signing-key-outside-source.pem \
  --upload-url https://THE_TEMPORARY_WORKER.workers.dev \
  --expected-sequence 0 \
  --lock /absolute/private-signing-directory/publish.lock
```

Use `--expected-sequence 0` only while the channel pointer is absent. Later
publications must specify the actual observed sequence. A failed final response
can mean a complete pointer write became visible: inspect the live channel
before retrying. An exact already-published release is idempotent when its
current sequence is explicitly supplied.

Every temporary bridge request carries an Ed25519 permit over
`noventra-r2-upload-v1\n` plus canonical JSON. The permit binds the exact HTTPS
origin, method, route, action, object key, body size/hash, expiry of at most five
minutes, and any multipart identity/part list or pointer precondition. Canonical
equality and exact action schemas reject duplicate/unknown fields. Domain
separation prevents an upload permit from being used as a signed update manifest.
No private key or reusable bearer credential is sent to the Worker.

Files larger than 8 MiB use signed, hashed 8 MiB parts. Multipart objects are
content-addressed; the client then reads and hashes the **complete remote object**.
All immutable objects are verified before the channel pointer is written last.
R2 conditional writes enforce initial pointer absence, later pointer ETag
compare-and-swap and immutable small-object creation. A local private lock also
prevents concurrent publishers on this machine. Failed known multipart uploads
are aborted; if a begin response is lost, R2's automatic incomplete-upload expiry
is the fallback. No endpoint lists or deletes arbitrary bucket objects.

An optional `--token-file` or `CLOUDFLARE_API_TOKEN` backend streams through the
fixed Cloudflare REST API origin. It never follows redirects. That management
API has no documented object compare-and-swap, so publishers using that optional
backend must be serialized across machines. The actual no-token setup should use
the temporary signed bridge instead.

## Verification and deployment boundaries

Run from the repository root:

```sh
python3 -B -m unittest discover -s tests/noventra -p test_publish_release.py -v
node --test tests/noventra/test_release_site.mjs tests/noventra/test_release_upload_worker.mjs
CI=true npm exec --yes --package=typescript -- tsc -p noventra/release-site/tsconfig.json
```

Regenerate binding/runtime declarations after config changes with the installed
Wrangler version, then dry-run both modules before deployment. Generated types
come from Wrangler 4.156.1. The tests use disposable keys, fake browser payloads
and an in-memory R2 implementation; the Python-to-JavaScript permit check uses
the actual OpenSSL signer and WebCrypto verifier. They do not establish live
Cloudflare permissions, a successful real upload or browser-native behavior.
The parent release qualification must verify the actual public HTTPS manifest,
signature, archive/source hashes, fresh install and later update flow.

The landing page has no scripts or trackers. Fonts, stylesheet and images load
from the main Studio origin. Its labels are escaped, metadata reads are bounded,
downloads stream with byte ranges and
ETags, update metadata is not cached, and immutable archives are cacheable.
Missing/malformed release metadata returns an honest preparation page or a
closed failure. Neither Worker changes browser profiles or launches a browser.

Primary platform references: [R2 Workers API](https://developers.cloudflare.com/r2/api/workers/workers-api-reference/),
[Workers best practices](https://developers.cloudflare.com/workers/best-practices/workers-best-practices/),
and the existing [signed delivery protocol](../../docs/noventra/update-protocol.md).
