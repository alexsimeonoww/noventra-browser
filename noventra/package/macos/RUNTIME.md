# Native runtime findings, November 4, 2026

These findings apply to the eight-patch native candidate with manifest
`ac3892eb84990a54b28f9bb1a8d69852959e27376cdcc2a9391422301946e9f9`.
The current source has sixteen patches and 677 declared paths. All twelve
previous Mac patches are preserved, with four shared Linux 0.1.8 deltas added.
The combined layer passes source replay. It has not been compiled or qualified
on the Mac. The findings below remain historical evidence for the eight-patch
candidate.

Native evidence was collected on an Apple silicon Mac. Its owned compiler
workspace and test profiles remain outside this source kit.

The untouched `compiler-r1-03/src/out/Default/Noventra.app` passes native
`--version` and a local HTML JavaScript DOM test. The final harness run reports
exit 0 for both. The DOM test changed `PENDING` to `NOVENTRA_NATIVE_JS_42` and set
`data-noventra-smoke="passed"`. It completed in 1.130 seconds. These checks use
fresh owned incognito profiles, normal sandboxing and mock Keychain.

The existing `package-r1-03-03/Noventra-arm64-test.dmg` remains unqualified.
Its copied-out app aborts with SIGABRT before startup. The exact dyld error is
`mapping process and mapped file (non-platform) have different Team IDs`.
All 16 actual generated signing targets have coherent ad-hoc signatures, no
TeamIdentifier, correct nested signing order and the generated JIT entitlements.
There is no evidence of inconsistent identities or incomplete nested signing.
The source app has linker ad-hoc signatures without the package's added
library-validation/hardened-runtime flags. That explains why its version
command can run while the protected ad-hoc package aborts.

Apple library validation accepts Apple code or code signed with the same team
as the executable. Team identifiers are recorded by suitable Apple-issued
certificates. Ad-hoc signing supplies no such team. The actual failure, manual
and Apple documentation support an inherent incompatibility for this app's
ad-hoc signing route while these protections are preserved.

The keychain search found one valid code-signing identity. It is self-issued,
has no Apple issuer or team field, and is unsuitable. There are zero suitable
Apple Development, Mac Developer or Developer ID Application identities. No
private-key operation, certificate creation/import, keychain unlock/ACL/trust
change, enrollment, notarization or publication was performed. A suitable
existing Apple identity with authorized private-key use is the external
prerequisite. The new identity route is regression-tested but cannot be
qualified natively until that prerequisite exists.

## The headless timeout

The original regular-profile `--dump-dom` command reaches a loaded local page.
Owned-process sampling shows the main event loop waiting. A loopback CDP
inspection reports `document.readyState="complete"` and the correctly mutated
DOM. The timeout therefore is not a startup crash, bad file URL, failed JS
execution or evidence that display-link warnings block startup.

Chromium implicitly adds `--incognito` in headless mode only when
`--user-data-dir` is absent. The original harness supplied an owned profile and
therefore disabled that implicit private launch. This port defaults to
`SessionStartupPref::LAST` on Mac. The regular session restore path can create
the target tab without entering `OpenTabsInBrowser`'s headless command handler.
No `chrome://headless` handler target appeared in the stalled run. Explicit
incognito disables session restoration and the original dump command finishes
with both DOM markers. This source trace and the controlled launch support the
session-restore routing diagnosis. The general regular-profile headless command
behavior remains a product limitation; no C++ source change is claimed.

Pre-seeding a fresh profile's tracked startup preference did not fix the
regular-profile probe. The preference was absent after startup. That experiment
is retained in evidence and is not part of the qualification harness.

## Implementation and checks

`qualify_macos.py` uses subprocess argv, a file URI produced by `Path.as_uri`,
explicit incognito and unique owned profiles. Timeout is independent of the
process's eventual exit status. Both version and JS-produced DOM markers must
pass. It terminates only its owned process tree by PID and records remaining
owned processes. Its regression suite checks URL escaping, missing markers,
nonzero exits, timeout cleanup and preservation of an unrelated process.

The integrated qualifier also refuses non-Darwin or non-arm64 hosts before
reading application data or creating runtime evidence. That guard has an
additional offline regression; the native results above used the earlier
harness on the independently identified arm64 Mac.

`package_macos.py --signing-identity "$NOVENTRA_SIGNING_IDENTITY_SHA1"` accepts
only an exact available appropriate identity. A disposable executable copy must
sign successfully and satisfy the Apple trust anchor and exact selected leaf
certificate. All staged and DMG copy-out code must retain the actual team,
generated flags, requirements and entitlements. Staged and copy-out runtime
checks must pass before a successful package receipt can claim
`runtime_verified=true`. Ad-hoc signing is refused before creating output.
No runtime/library-validation flags are removed and no protection exceptions
are added.

Run from the derivative kit root:

```sh
python3 -B tools/noventra/build_macos.py check-layer
python3 -B -m unittest discover -s tests/noventra -p 'test_*macos.py'
python3 -B tools/noventra/qualify_macos.py \
  --app /path/to/owned-build/Noventra.app \
  --output /path/to/new-runtime-evidence
```

The host evidence directory is `evidence/runtime-r2`. Important receipts are
`signing-audit.json`, `identity-availability.json`, `native-packager-refusal.json`,
`compiler-qualified-final/runtime-receipt.json`, and
`dmg-recheck/runtime/runtime-receipt.json`. Historical failed probes and
their logs remain available. An initial 8-second sampling attempt timed out;
its owned processes were explicitly cleaned up. A subsequent bounded sample
completed successfully.

## Qualification limits

Mock-Keychain headless checks do not qualify GUI flows, real Keychain, imports,
onboarding, password-manager companion integration or transfer. The shared
setup transfer-entry and import infrastructure are now present as source.
Cookie import and independent-profile Spaces retain their Linux gates until
native Mac implementations are verified. Mac private Spaces retain the genuine
private-window fallback. Mac auto-update remains
disabled. The old DMG is ad-hoc, unnotarized and unusable. No new runtime-qualified
DMG was produced because the required Apple identity is absent.

Primary references are Apple's [Code Signing Tasks, Using Library Validation](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/Procedures/Procedures.html),
[library-validation entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.disable-library-validation),
and [launch environment and library constraints](https://developer.apple.com/documentation/security/defining-launch-environment-and-library-constraints).
The local `codesign(1)` manual and pinned generated Chromium
`signing/parts.py`, `signing/model.py` and `signing/signing.py` were also read.
