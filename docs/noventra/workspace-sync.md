# Workspace settings sync

Noventra's workspace sync connects to an explicitly configured local or self-hosted
object store. The browser encrypts the workspace document before uploading it,
using the inherited `SyncVaultStore` and its authenticated journal. The server
stores opaque bytes and exact revisions; it never receives the recovery key.
No default public server is configured.

This feature covers space names, order, archive state and the saved dock layout.
Tabs, drafts, cookies, logins, passwords, history, domain-routing rules and other
browser settings remain local. It is separate from the settings JSON backup and
from Chromium's general account sync.

## Start an owned local store

Use Python 3 with its standard-library SQLite and TLS support. The following
commands create a private store directory and a random access token. Choose a
new directory; do not overwrite an existing token used by other clients.

```sh
umask 077
mkdir -m 700 /absolute/path/to/noventra-sync
python3 -c 'import secrets; print(secrets.token_urlsafe(32))' \
  > /absolute/path/to/noventra-sync/access-token
python3 -B tools/noventra/sync_store_server.py \
  --store /absolute/path/to/noventra-sync/store \
  --token-file /absolute/path/to/noventra-sync/access-token \
  --port 8766
```

The token file must be owned and private (`0600`). The data directory must be
owned and private (`0700`), with no symlink at the supplied directory or database
path. The server does not print its token or request bodies. The token is a bearer
credential: keep it outside a served web directory and transfer it privately.
All clients using this store share its access authority. This is a single-owner
store, not a multi-tenant account service.

The default origin is `http://127.0.0.1:8766`. Omitting `--port` selects an available
port and prints the actual origin. Stop the owned server with Ctrl+C. Restarting
with the same directory and token preserves ciphertext and revisions. There is
no installation, desktop registration or persistent system service in this command.

For other devices, serve through HTTPS with a certificate trusted by those
devices. The server can terminate TLS itself:

```sh
python3 -B tools/noventra/sync_store_server.py \
  --store /absolute/path/to/noventra-sync/store \
  --token-file /absolute/path/to/noventra-sync/access-token \
  --host 0.0.0.0 --port 8766 \
  --tls-cert /absolute/path/to/fullchain.pem \
  --tls-key /absolute/path/to/server-private-key.pem
```

Non-loopback binding requires TLS. An owner-configured HTTPS reverse proxy to a
loopback listener is another option; preserve authorization, conditional-write
headers, response ETags and raw binary bodies. Do not cache object responses.
Do not bypass certificate verification. Domain ownership, certificates, remote
firewall/access configuration and remote deployment are separate setup steps.

## Connect browsers

In **Settings → Data & devices → Advanced: workspace settings only → Self-hosted workspace settings sync**, choose
**Create a new vault**, enter the server origin and access token, and create it.
Save the displayed vault ID and recovery key privately before closing Settings.
The recovery key is shown only as part of creation; routine status never exposes
it or the server access token.

On another device, choose **Join an existing vault** and enter the same origin,
access token, vault ID and recovery key. Joining applies the vault's saved layout
and Personal name. It creates separate local account partitions for the other
synced spaces. Existing non-Personal spaces on the joining device remain local,
including their cookies and open work. Spaces created after enrollment join sync.
Names are never used to merge account identities.

Subsequent changes update the same local spaces using a durable mapping from
vault identity to local partition identity. They do not create a fresh copy on
each sync. Archiving a synced space preserves its local tabs and login data and
uses the existing Personal fallback. A space omitted by conflict resolution is
kept locally instead of having its account data deleted.

Sync observes committed settings, polls the configured store and offers **Sync
now**. It does not upload an unapplied layout-editor draft. If both sides changed
since their common state, it retains a conflict and asks whether to keep this
device's version or use the vault's version. There is no timestamp-based silent
overwrite. Resolution checks the current authenticated remote head again.

A device with a long saved history catches up in bounded steps and continues
automatically. The progress message is not a completed sync: the last successful
sync time and trusted checkpoint stay unchanged until the history and current
settings are fully verified. A restart safely discards unfinished in-memory
progress and starts validation again from the durable checkpoint.

**Disconnect** cancels synchronization and keeps both local spaces and the remote
vault. It does not delete either. Losing or corrupting remote data is an error,
not permission to initialize a replacement vault while joining.

## Storage and protocol limits

The browser uses OS-protected local state for its recovery key, access token,
authenticated checkpoint, identity mapping and reconciliation state. On Linux,
setup requires a protected Secret Service or Secret Portal provider. The Basic
password store is rejected before enrollment or network requests. Unlock the
desktop key store before configuring sync; there is no insecure-storage bypass.

The pinned Linux providers use `v11` and `v12` encrypted-state prefixes. Loading
`v11` state additionally verifies that the current protected key reproduces the
ciphertext, excluding the inherited public-key fallback. This conservative check
does not support switching an existing `v11` connection to a selected `v12`
provider. Keep the original provider available, or disconnect and rejoin using
the separately saved recovery key. Disconnect preserves local workspaces and
account data; joining treats preexisting non-Personal spaces as local-only.

The server accepts authenticated GET/PUT requests under
`/v1/vaults/<uuid>/objects/<base64url-object-id>`. IDs are canonical encodings;
decoded object IDs are opaque UTF-8 strings, not filesystem paths. Creation uses
`If-None-Match: *`; replacement requires the exact strong `If-Match` ETag. The
condition check and write share one SQLite transaction. Failed conditions return
412 and leave the object unchanged. Synchronous SQLite commits preserve completed
writes across normal server restart.

Limits are 16 MiB per object, 4 KiB per decoded object ID, 100,000 objects and
1,000 vaults. Total ciphertext quota defaults to 1 GiB and can be explicitly set
with `--quota-bytes`. Reads have a 30-second socket timeout and at most 32 concurrent
request threads. Rejected or malformed requests cannot bypass the quotas.

This first store does not collect old encrypted journal objects, so usage grows
with changes. Quota exhaustion reports an error and preserves existing data.
Do not manually remove objects by guessing their meaning or age. Back up the
store consistently (for example, stop it before copying the private database),
and retain the recovery key separately. Restoring an old server backup can be
detected as a rollback by a client's authenticated checkpoint; that protection
must not be bypassed by silently resetting the client checkpoint.

The encrypted vault's wire format retains upstream compatibility identifiers.
Those identifiers and upstream license credits are not a configured upstream
service. Traffic goes only to the origin the user explicitly supplies.
