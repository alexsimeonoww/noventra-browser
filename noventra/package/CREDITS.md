# Source and packaging credits

Noventra is derived from Helium and Chromium. The inherited Linux packaging
templates and launcher code include work by The Helium Authors; their copyright
notices are retained. Helium's upstream Linux project is
https://github.com/imputnet/helium-linux and Chromium's source project is
https://www.chromium.org/. These are upstream references, not Noventra support,
release, or update services.

Noventra source is at https://github.com/alexsimeonoww/noventra-browser.
The root project `LICENSE` retains the GNU GPLv3 license; Chromium and bundled
dependencies retain their own license notices. Public Linux x86_64 previews
and their source & build kits are offered through
https://downloads.noventrastudio.com/. macOS and Windows downloads are not
available yet.

The supplied snapshot's `LICENSE-Helium`, `LICENSE-Chromium`, and any additional
license notices are preserved verbatim beside the runtime. Its `provenance.json`
and `snapshot.json` identify the supplied build and its file hashes. Provenance
records alone are not a bundled source tree, signature, or reproducible-build
proof. A public release's source & build kit provides the exact Noventra changes,
build inputs and pinned upstream source references alongside the release's
source identity. It is not a bundled Chromium source tree or a claim of
reproducible builds. Preserve upstream licenses and notices when redistributing.

Public preview packages include a pinned configuration for signed Noventra
updates. The standalone packaging tool also supports manual packages without
an updater and explicitly configured forks; it does not publish a release or
create a server. Update signatures authenticate release delivery, while the
Linux AppDir remains unsigned by an OS application-signing service.

The desktop icon comes from the approved Noventra artwork. The package records
the generated icon manifest and source artwork hash; it does not fetch artwork.
`local.noventra.browser` is a local metadata identifier, not a domain ownership,
signing team, app-store registration, or update-service claim.
