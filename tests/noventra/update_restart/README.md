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
