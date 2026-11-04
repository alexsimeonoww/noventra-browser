# Sidebar space capacity and floating resize

Linux preview 5 corrects the space-control capacity in the expanded sidebar.
At the default width, three Space shortcuts, including Incognito, fit beside
the Bookmarks and More controls. The capacity calculation now uses the available space-control width
without subtracting the footer padding again. Normal overflow remains available
through More, and active/private space prioritization remains in effect.

The follow-up also fixes animation completion when a callback synchronously
starts a replacement motion. That could leave the animation group using stale
completion state and crash while a floating sidebar was narrowed. Cancellation
and completion now preserve the replacement motion and avoid duplicate ending
callbacks.

## Verification status

The preserved native candidate passed 536 checks across X11 and software-rendered
Wayland. They cover floating and pinned sidebar resizing, both sidebar sides,
three Space shortcuts beside Bookmarks and More, compact/overflow layouts,
native browser-menu Incognito, retained documents and drafts, last-private-tab
cleanup, and pinned close/unload behavior in both tab layouts.

All 44 focused C++ animation-controller tests passed, including replacement
motions started by completion and clear callbacks. The test binary uses the
current production controller objects without product stubs. Native Mac/Windows builds, hardware GPU qualification and the full Chromium
suite are outside this follow-up's qualification.

The native regression source is `tests/noventra/sidebar_narrowing_smoke.py` and
`tests/noventra/space_capacity_resize_smoke.py`. It requires the owned private
display and namespace harness described by the
[native fixture notes](../../tests/noventra/update_restart/README.md).
