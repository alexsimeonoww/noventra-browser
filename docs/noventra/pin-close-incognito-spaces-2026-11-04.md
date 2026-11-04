# Closing saved pins and returning to Incognito

Linux preview 4 keeps the saved shortcut when a normal pinned page closes.
Hover or focus the pin to reveal its X, or press Ctrl+W. Clicking the shortcut
loads its last page again with its custom name, saved home and original account.
Use **Unpin** to turn it into an ordinary tab. Closing the only loaded pin opens
an ordinary New Tab in that account. A site's leave confirmation still offers
Cancel and Leave. Closing one pane of a pinned split preserves the other pane.

This uses Chromium's explicit tab-discard path. Its small tab descriptor and
saved navigation remain; its loaded page is released. Shared renderer or service
worker memory is not guaranteed to disappear. Saved pins remain present after a
normal restart, with the existing lazy background-tab loading policy.

Create Incognito through the browser menu or Ctrl+Shift+N. A live Incognito
shortcut remains reachable when returning to Personal, including a crowded
sidebar or compact rail. Clicking it returns to the same loaded private document
and draft. Closing its last tab removes the temporary space and its private
storage. Private pins retain ordinary private-tab closing behavior so they do
not prevent last-tab cleanup. The existing four-dot rail limit remains;
additional spaces are available in More.

## Verification scope

The preview 4 candidate passed 185 native checks on private Linux X11 displays:
23 sidebar pin checks, 23 shipping horizontal-tab checks, 6 expanded Incognito
checks, 8 compact Incognito checks, 50 pin lifecycle checks and 75 pin editing
checks. All nine browser sessions exited normally. Both regular and private
rails measured 48 DIP in the compact case. Pin X and private shortcut captures
were inspected. Full Chromium suites, Wayland/GPU, Windows and native Mac builds
were not run for this delta.

The regression source is included as `tests/noventra/pin_close_smoke.py`,
`pinned_tabs_smoke.py` and `incognito_space_visibility_smoke.py`. Their private
native harness requirements are documented in the
[native fixture notes](../../tests/noventra/update_restart/README.md).
Discard can replace a WebContents/session tab ID; tests inspect browser-owned
pin membership and saved metadata without evaluating an unloaded page.
