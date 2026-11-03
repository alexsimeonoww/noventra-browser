# Import cookies and signed-in sessions

Cookie import is an optional part of browser-data import. Select **Cookies and
signed-in sessions** in setup or the Settings import dialog. The choice starts
unchecked. During setup, the selected data is copied when setup finishes.

The import writes to the regular Space from which it was started. Cookies
already present when import starts take precedence over matching source cookies.
Other Spaces keep their separate cookie stores. Incognito is not an import
destination.

Some websites still require you to sign in again. Cookies do not contain every
kind of website storage or protected session credential. This feature does not
copy local storage, IndexedDB, passkeys, device-bound private keys, or browser
extensions.

## Supported source formats

The Linux preview supports the known Chromium cookie formats used by Chrome,
Chromium, Brave, and Helium, including Linux `v10` and `v11` encryption. A `v11`
import can ask the desktop credential store to unlock the source browser's
existing key. The importer never creates a missing source key or tries keys
from other browsers. Modern cookies' encrypted domain verification value must
match the cookie's domain.

Firefox cookie schemas 15 and 16 are supported. Schema 15 stores expiry in Unix
seconds. Schema 16 stores it in Unix milliseconds. Unknown schemas fail with a
message rather than guessing their layout.

Expired cookies and cookies with unsupported storage isolation are skipped.
This includes partitioned cookies and Firefox container cookies. The importer
preserves supported domain, path, Secure, HttpOnly, and SameSite restrictions.
It does not remove isolation or security attributes to make a cookie importable.
The result reports imported and skipped counts.

These are Linux-qualified source readers. Windows and macOS cookie import are
not claimed to be qualified by the Linux release.

## Source and destination handling

The importer snapshots source databases and their WAL or journal sidecars into
an owned private directory. It verifies the source bytes remain stable during
the copy. SQLite recovery runs on that copy. The original files are read only.
Close the source browser and retry if it changes the database during copying.

Cookie rows are read and validated before destination writes begin. Reads and
native transport are bounded. Unsupported encryption and failed domain checks
do not fall back to raw database copying or other keys.

Cookie values travel through the native import connection and cookie manager.
The setup and Settings pages receive counts and status, not cookie values.
Completion waits for cookie-write callbacks and a cookie-store flush. Cancelling
an import may leave cookies already copied; it must not report the operation as
fully finished. A website can still change its own cookies during an import.
The cookie manager does not provide an atomic transaction covering those
concurrent website writes.

## Native verification

`tests/noventra/onboarding_cookie_import_smoke.py` uses disposable browser
profiles, synthetic cookie values, and owned local HTTP and HTTPS endpoints.
Its cases cover setup consent and completion, Linux encryption, supported
Firefox schemas, Space isolation, failures, and cancellation. An endpoint
recognizes an imported session without setting a replacement cookie during the
proof. Tests never read personal profiles or contact the personal credential
store.

The native fixture runs through the project's private display and namespace
workflow. A source-data preflight alone does not qualify the browser. Release
evidence must bind passing native results to the actual compiled binary,
resources, source freeze, and distributed package.
