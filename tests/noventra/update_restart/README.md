# Native update and setup fixture source

`run.py` and `dialog.mjs` are the exact fixture bytes used for the preview 3
candidate's 35 native update checks. Their directory depth preserves the
fixture's repository-root calculation. The corresponding setup handoff fixture
is `../onboarding_device_transfer_smoke.py` and covers 17 checks, including
native Light/Dark palettes and narrow layouts. The exporter includes their
local Python and helper source dependencies.

The update fixture creates two explicitly labelled local packages from the
same candidate ELF, signs disposable loopback feeds, and exercises the real
About button, cancellation prompts, managed restart and atomic installation.
It checks signed sequence state, complete installation inventory, restored
tabs and separately retained Space cookies/local storage. This is not an
older-native-to-newer-native comparison. Reused signed fixture archives, when
requested, are independently verified and used with a fresh profile.

These GUI fixtures require the project's owned private Xvfb/EWMH environment,
Pillow, Node 24, X11/XTest/ImageMagick, the Linux user/network namespace tools,
and a preserved browser runtime snapshot. The fixture guards intentionally
expect the private display adapter under `work/ram-investigation` and reject a
normal desktop session. The private adapter's extracted Xvfb/Fluxbox payloads,
compiler outputs, profiles and diagnostic captures are not distributed in this
source kit. These files are reviewable regression source, not a turnkey public
GUI test command. Do not remove their ownership checks or point them at a
personal profile or desktop to make them run.

The public `.github/workflows/noventra-checks.yml` workflow instead runs the
self-contained package, signed-delivery, desktop-updater, recovery, publisher,
GitHub helper and hosting unit tests. Those tests use disposable inputs and
their own test signing keys; they do not need the private GUI adapter.

## Saved pin and Incognito shortcut regression source

`../pin_close_smoke.py` and its helper `../pinned_tabs_smoke.py` cover native
closing/unloading, clicking a saved pin to reload it, beforeunload Cancel/Leave,
pinned split retention, account-correct replacement New Tab and restart metadata.
`../incognito_space_visibility_smoke.py` creates Incognito through the actual
browser menu, then verifies its shortcut in crowded and 48-DIP compact regular
sidebars, exact private document/draft retention and last-tab removal.

The preview 4 candidate passed 185 native Linux X11 checks across those new
cases and the existing pin lifecycle/editing cases. The exporter includes these
fixtures and their Python/helper closure for review. Their private display and
namespace ownership requirements remain in force; they are not public CI jobs.
No Windows, native Mac or Wayland/GPU result is implied by these X11 checks.

## Sidebar capacity and floating narrowing regression source

`../space_capacity_resize_smoke.py` exercises the three-space footer at the
default expanded sidebar width with Bookmarks and More, plus native resizing.
`../sidebar_narrowing_smoke.py` exercises repeated narrowing in the floating
sidebar. Their local helper closure is inventoried with this source kit.
The preserved preview 5 candidate passed 536 native checks across owned X11
and software-rendered Wayland displays. All 44 focused animation-controller
C++ tests also passed using the current production controller objects. The
Wayland wrapper, pointer/keyboard clients and protocol definitions are included;
they require the system dependencies documented by `../with_private_sway.py`.
No native Mac/Windows build or hardware GPU qualification is claimed.
