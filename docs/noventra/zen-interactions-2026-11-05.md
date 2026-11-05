# Centered new-tab input and preview controls

**New Tab** opens the native URL and search field in the center of the current page. Typing leaves the current document and tab count intact. Pressing **Enter** opens the accepted URL or search in a new foreground tab. **Escape**, clicking outside, or pressing **Ctrl+T** again cancels without creating a blank tab.

The keyboard shortcut, browser menu, sidebar plus button, horizontal plus button, and mirrored window plus button use this flow. The field retains native suggestions and the selected search provider. Ordinary **Ctrl+L** continues to edit the current tab. Programmatic tab creation, startup pages, and contextual commands that choose an insertion position retain their existing behavior.

`NoventraDockController` owns a current-tab or new-tab edit intent. `ShowNewTabPopup` opens or cancels the existing native field. `OpenCurrentURL` resolves an accepted current-tab disposition to a new foreground tab before navigation can dismiss the field. The browser's existing navigation path supplies the active Space or private profile.

`ToolbarView::NewTabButtonPressed` serves the movable button that Noventra places in the sidebar. That callback bypasses both `IDC_NEW_TAB` and `BrowserActions`, so each route calls the same popup helper.

Link previews have three separate round buttons, ordered **Close**, **Expand**, and **Split**. The preview no longer displays Back and Forward buttons. Expand uses the fullscreen glyph. The transparent control area uses 12-pixel padding and gaps, with 32-pixel buttons positioned outside the page. Promotion and split continue to move the existing preview document.

## Zen reference

The comparison uses Zen's source at commit `e5f9d1e620065f4154ed8ebaef2138b1594b9fc4`.

- [Glance button order and actions](https://github.com/zen-browser/desktop/blob/e5f9d1e620065f4154ed8ebaef2138b1594b9fc4/src/zen/glance/zen-glance.inc.xhtml).
- [Glance geometry and individual round surfaces](https://github.com/zen-browser/desktop/blob/e5f9d1e620065f4154ed8ebaef2138b1594b9fc4/src/zen/glance/zen-glance.css).
- [Floating URL field and new-tab intent](https://github.com/zen-browser/desktop/blob/e5f9d1e620065f4154ed8ebaef2138b1594b9fc4/src/zen/urlbar/ZenUrlbarController.mjs).

Noventra reuses its own native field, theme, hover, and focus behavior. This change does not reproduce every Zen animation or mirror the preview controls with sidebar placement.

## Verification

`tests/noventra/new_tab_popup_smoke.py` drives the actual browser with native input in disposable profiles. Its cases cover URL and search submission, cancellation, keyboard and plus-button routes, current-tab editing, Space storage, and incognito storage.

`glance_sizing_smoke.py` checks the native preview controls, resize behavior, Close, Expand, Split, and document retention. `glance_workflow_smoke.py` covers pinned source and split workflows. `glance_floating_gpu_smoke.py` contains the Wayland transparency checks.

The private display, network, and GPU wrappers remain local prerequisites. Native fixture source is included for review.

The ordered Chromium patch layer includes both interactions in `noventra-zen-interactions.patch`. The source manifest inventories the additional stock tab-strip controller file and its pristine hash.

The final Linux candidate passed 364 native assertions across seven cases. These include 127 new-tab checks over three layouts, 26 Space and incognito checks, 139 preview workflow and geometry checks, and 72 Wayland GPU checks. Captures confirmed the centered field and separate round preview buttons. macOS, Windows, HiDPI, screen readers, and mouse selection of suggestions were not qualified in this task.
