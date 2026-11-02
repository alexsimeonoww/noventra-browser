# Open source and GitHub releases

Noventra's browser source can live in an independently owned public GitHub
repository. Keep the inherited **GNU GPL version 3** license and original
copyright notices; Chromium and other dependencies retain their own licenses.
The repository's existing upstream remote identifies Helium Linux. Creating a
Noventra repository does not require changing that upstream remote or changing
any other project's visibility. Review a clean export before its first public
commit, excluding profiles, credentials, private signing material, compiler
outputs and personal captures.

The public source repository is
[`alexsimeonoww/noventra-browser`](https://github.com/alexsimeonoww/noventra-browser).
The source repository and GitHub Releases provide the public development/release
record. Cloudflare at `downloads.noventrastudio.com` remains the browser's pinned
signed update origin; the same verified files are mirrored on GitHub.

## Source commit and build kit

Use the same explicit source/build inventory for the qualified binary, its
downloadable kit and its public source commit. The Linux kit contains all
Noventra-modified prepared files, build inputs, exact upstream pins, compiler
arguments and build instructions. It does not bundle the entire Chromium
dependency tree or prove byte-identical reproduction.

The default public repository layout flattens the kit's `build-inputs/` into the
repository root and retains `prepared-source/` beside it. Commit the kit's exact
generated `source-identity.json` bytes at `noventra/source-identity.json`.
Include the inherited `LICENSE` at the root and the exact approved **public**
update key as an inventoried build input. A fork must provision its own signing
identity and endpoints; private keys never belong in source or release assets.

`tools/noventra/github_release.py` verifies every inventoried ZIP entry's bytes,
size and mode, allowing only the generated kit README and identity as extras.
It computes the corresponding Git blob IDs and verifies them against the exact
remote commit tree, including the committed identity. The existing remote tag
must resolve to that full commit SHA. It also verifies the signed archive and a
passed Linux qualification receipt identifying the same binary, resources and
source freeze. An arbitrary commit string or version label cannot establish
that correspondence.

The helper never creates or moves a tag. Create the reviewed source commit and
its tag explicitly before preparing a GitHub Release. A tag such as
`vVERSION-rSEQUENCE` also leaves room for signed-manifest renewals. Do not reuse a
published tag or asset filename for different bytes. Where available, enable
GitHub's immutable release protections and protect release tags. The helper
reports GitHub's actual `immutable` flag; merely naming a tag does not make it
immutable.

## Compile, qualify, sign, mirror and activate

1. Review and commit the clean source and source identity to the owned public
   repository. Ordinary development commits are allowed before a browser release
   is ready; they do not ship binaries.
2. Compile in the controlled native build environment. Chromium needs hundreds
   of GB of source/toolchain/build storage. Standard hosted GitHub runners are
   used for the small tool tests below, **not** claimed to compile this browser.
3. Preserve the exact runtime and qualify the native UI/privacy behavior,
   package, install and update flows. Keep the passing receipt and source freeze.
4. Build the matching source kit. Prepare the signed public release with
   `publish_release.py prepare`, an explicit increasing sequence and the offline
   private signing key. See [release hosting](../../noventra/release-site/README.md).
5. Run the read-only GitHub check, upload a draft, inspect it, then explicitly
   publish the verified draft. The helper uses the authenticated `gh` CLI and
   never reads or emits its token.
6. Upload the exact same prepared release to R2 with the signer-authorized
   temporary bridge. It verifies all remote bytes before the channel pointer is
   committed last. Remove the temporary Worker afterward. Verify public HTTPS
   and the actual client check/download/reopen flow.

The GitHub commands take identical explicit inputs; only the operation changes:

```sh
python3 -B tools/noventra/github_release.py check \
  --release /absolute/prepared-public-release \
  --public-key /absolute/approved-public.pem \
  --qualification /absolute/final-linux-qualification.json \
  --repo alexsimeonoww/noventra-browser \
  --commit FULL_40_CHARACTER_SOURCE_COMMIT \
  --tag vVERSION-rSEQUENCE
```

After a successful check, use `draft` with those same arguments. It creates an
unpublished prerelease and uploads all exact prepared files, `SHA256SUMS`,
`noventra-release-provenance.json` and a sanitized qualification summary.
The provenance binds repository/tag/commit/tree, source identity, source archive,
binary snapshot and qualification digest without publishing local profile paths.
GitHub's computed SHA-256/size for every asset must match. Identical retries
reuse existing assets; conflicting, incomplete or unexpected assets fail closed.
No clobber or deletion is attempted. A failed upload leaves the draft available
for inspection and exact retry.

Use `publish` with the same arguments only after reviewing that complete draft.
It refuses to create or fill an incomplete draft, rechecks the source tag and
publishes only verified assets. If the final network response fails, inspect
GitHub before retrying: publication may already have succeeded. An identical
already-published release can be verified again without mutation.

Manifest expiry remains part of the update protocol. Even a renewal with the
same binary must use a larger sequence and a new GitHub release tag; original
published assets remain unchanged. The process does not silently convert source
commits into releases or install unqualified builds.

## Public pull request checks

`.github/workflows/noventra-checks.yml` runs small package, signed-delivery,
desktop-updater, publisher and GitHub-helper tests plus hosting/upload tests on
an ephemeral GitHub-hosted Ubuntu runner. Official actions are pinned to exact
verified commits. The workflow grants only `contents: read`, does not persist
checkout credentials, has no secrets or deployment jobs, and never runs pull
request code on a self-hosted machine. It does not build, sign, upload or publish
a browser. Native Linux/macOS/Windows release qualification remains separate.

The full tool suite needs Python 3.11+, OpenSSL, Node 24, desktop-file-validate,
xdg-utils, GIO, and a C compiler with libc development headers. The workflow
installs these dependencies explicitly. Process tests compile a tiny standalone
sleeping program in a disposable directory to check real executable identity,
launch locks and shutdown. Its argument handling is independent of renamed
system utilities; native browser qualification remains a separate step.

The helper tests mock the GitHub boundary and use disposable signing keys and
fake browser payloads. They do not establish actual repository permissions or a
successful public release. Run them locally with:

```sh
python3 -B -m unittest discover -s tests/noventra -p test_github_release.py -v
```

Primary references: [GitHub Releases API](https://docs.github.com/en/rest/releases/releases),
[release asset digests](https://docs.github.com/en/rest/releases/assets), and
[minimum workflow token permissions](https://docs.github.com/en/actions/tutorials/authenticate-with-github_token).
