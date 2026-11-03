# Device transfer

Move selected data between Noventra installations with an encrypted file
or a one-time direct connection.

## Settings and setup

Open **Settings → Data & devices**. The setup Data step offers **Move from another
Noventra PC** above imports from browsers on the current PC. **Open transfer**
opens this same section in a separate tab, where people can connect the two PCs
with codes or restore an encrypted `.noventra` file. Setup stays open with its
selected browser data queued; return to it after reviewing and importing the
transfer. The welcome-page shortcut remains available too.

Opening the transfer flow does not mark setup as finished or start importing
data automatically. The existing review and confirmation still apply.
Transfers are optional and available in a
regular local browser profile, without a Noventra account or subscription.

The first version supports an encrypted file and direct, one-time transfer.
It does not provide automatic background sync or reconnect after browser exit.
The existing self-hosted [workspace settings sync](workspace-sync.md) remains a
separate feature with narrower data coverage.

## What moves

Choose bookmarks, Spaces, open tabs and pins, appearance/layout, an extension
reinstall list, and optionally built-in saved passwords. Tabs require Spaces so
their account boundaries can be restored. The native snapshot records Space
order, archive state, colors, icons, optional per-Space themes and domain routes.
Older files without a Space theme remain readable. Use current builds on both
devices when transferring explicit themes; older receivers may reject that
optional field. The destination Personal Space keeps its existing appearance.

Imported bookmarks go under **Imported from Noventra**. Existing bookmarks,
tabs and Spaces remain in place. Imported non-Personal Spaces receive fresh
local storage identities and start signed out. Existing domain rules take
priority. Tabs restore URL snapshots and load when selected; unsaved forms,
split composition and back/forward stacks are not copied. The current tab stays
active. Appearance and layout are replaced only when selected; apply or discard
an existing layout draft first.

Local files, browser/debug pages, bookmarklets and unsupported URLs are excluded.
The review reports excluded counts. Store extension IDs are listed for reviewed
reinstallation; code, native hosts and extension permissions are not copied.
Built-in, managed and unpacked extensions are excluded. Password-manager vaults
remain with their vendors, and the extensions need sign-in/unlock again.

Website cookies, active login sessions, site storage, browsing history, passkeys
and password notes are not included. Signing into websites again on the new PC
is expected.

## Encrypted file

On the sending PC, choose the categories, enter and repeat a transfer password
of at least 12 characters, then choose **Save encrypted file**. The download has
the `.noventra` extension. Keep its password separately; losing it prevents
restoration. Password bytes, including capitalization and spaces, are preserved.

On the receiving PC, choose the file, enter its password and choose **Unlock and
review**. Nothing is imported during review. Select categories and confirm
**Import selected data**, or cancel. Reviews expire after ten minutes.

The same transfer can be retried without repeating completed categories. A
per-transfer local ledger also records restored-tab progress. Cross-store
application is not a crash-atomic transaction: partial outcomes must identify
completed work and let the user retry what remains.

## Direct transfer

Keep both browsers open on **Data & devices**:

1. Choose **Send from this PC** and copy its connection code privately.
2. On the other PC, choose **Receive on this PC**, paste the code and create a
   reply.
3. Paste that reply on the sending PC.
4. Compare the displayed device code on both PCs and confirm on each.
5. Send the selected data. The receiving PC then reviews and confirms its import.

Connection codes expire after ten minutes and contain one-time connection
secrets. There is no hosted pairing server or paid relay. Optional Cloudflare
STUN discovers connection addresses after starting a connection; it does not
carry the transfer payload. The checkbox can be disabled for a local-only
attempt. See [the service documentation](https://developers.cloudflare.com/realtime/turn/faq/).

WebRTC encryption and the authenticated encrypted envelope protect the payload.
The displayed comparison binds the session, random secret and both DTLS
certificate fingerprints. Both devices must confirm before payload transfer.
Some networks prevent direct connections. A failure offers the file alternative
instead of silently adding a relay or service dependency.

Noventra's inherited default WebRTC policy suppresses local candidates. Starting
a transfer temporarily permits candidate gathering for that trusted Settings
WebContents only when the saved policy is still the built-in default. Explicit
user, extension and organization settings retain their rules, as do per-URL
policies. Disconnect, unload or the native ten-minute expiry restores the
current policy. Saved preferences and ordinary websites are not changed.

## Passwords and limits

Built-in passwords are optional and stay in native memory during collection,
review and import. Decrypted CSV and browser credentials do not cross into the
Settings component or a readable intermediate file. Export uses the existing
platform authentication behavior: fresh native authentication on supported
Windows/macOS paths; the pinned Linux exporter has no OS reauthentication
prompt. This feature does not add one.

The native importer parses CSV in its existing utility sandbox. Existing
realm/username pairs and repeated source rows are kept, with no selected
replacement IDs. The store is refreshed again immediately before commit. This
does not add a cross-store atomic insert-if-absent primitive; simultaneous
credential writers retain the inherited importer's concurrency limits.

Snapshots are bounded to 32 MiB, 20,000 bookmark nodes, depth 20, 1,000 Spaces,
1,000 tabs, 100 domain rules and 1,000 extension records. Password data follows
the native 1,000 KiB / 3,000-row CSV limits. The versioned file uses fixed scrypt
parameters, random salt/nonce and Chromium's AES-256-GCM-SIV. Header, size and
version checks precede expensive key derivation. Wrong-password or damaged
envelopes produce no import plan.
