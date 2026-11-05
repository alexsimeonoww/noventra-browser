# Noventra workspace vault compatibility

This module retains the encrypted workspace settings format used by Noventra's
Chromium154 builds. It is independent of Chromium156's rewritten Helium sync
vault, whose descriptor/journal API and serialized messages changed. Existing
workspace vaults, checkpoints, cancellation and bounded incremental reads retain
their previous behavior. File export and direct device transfer use their own
existing formats.

The initial implementation comes from the preserved Noventra154 source based on
Helium b38c4bdd2ecbe5c680dc3c5d464a2edc84d49d4c, including the existing incremental
reader and its regression tests. Imported license and copyright notices remain.
The compatibility port changes include paths, guards, C++/protobuf namespaces and
the generated protobuf root. Protobuf field numbers, encryption context, object
roles, limits, transport revisions and checkpoint validation remain unchanged.

`noventra_sync_pb` prevents generated symbol collisions with the upstream vault;
its package name does not change these serialized protobuf messages. The shared
sync engine cancellation signal remains a `syncer::CancelationSignal`.

Build and run `//components/noventra_sync_vault:sync_vault_store_unittests` with the
matched Chromium toolchain. Keep the cursor, corruption, cancellation, ancestry,
changed-head and checkpoint regression checks when updating this component.
Any future format migration must authenticate the existing vault and preserve a
recoverable checkpoint before writing a replacement.
