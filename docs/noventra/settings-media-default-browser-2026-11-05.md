# Settings, default browser, and protected playback in Noventra 0.1.9

Noventra 0.1.8 corrects narrow Settings cards, interrupted Spell check expansion,
and default-browser selection with Omarchy's browser dispatcher. It also makes
tab-memory previews and optional protected-streaming setup easier to find.

To update a managed Linux installation, open **About Noventra**, check for
updates, then choose **Update and restart** when the download is ready.
Older installations can require a complete quit and reopen after staging.

Noventra 0.1.9 corrects the Linux desktop registration used by default-browser
selection. Its native browser and resources are identical to the qualified
0.1.8 release.

## Show memory for a tab

Open **Settings > Customize Noventra > Tabs & browsing > Tab previews** and enable
**Show tab memory usage**. Hover over a tab to see its estimate. New pages can
need a moment to report usage. **More tools > Task Manager** provides tab and
process details. These are estimates, not a complete accounting of browser RAM.

The existing memory preference and defaults are preserved. The control moves
out of More browsing options into the visible Tab previews group.

## Select the default browser

The native Make default action handles Omarchy's `BROWSER` dispatcher without
changing other browser overrides or URL-scheme handlers. The Linux installer
leaves simple Exec paths unquoted so Omarchy's launcher can discover them.
Paths with reserved characters retain Desktop Entry escaping.

The main desktop entry declares the `about` and `unknown` URL handlers used by
XDG's generic browser setter. Missing declarations caused XDG to move the MIME
list into a desktop action and invalidate the installer's ownership record.
The regression test selects the browser twice and verifies that registration,
permissions, and file identity remain intact.

An Omarchy default-browser chooser can use a fixed application list. Its
supported user-menu extension can add a Noventra entry. This browser release
does not rewrite another computer's Omarchy configuration.

## Enable protected streaming

1. Open `chrome://components/` in the address bar.
2. Under **Protected streaming**, enable **Allow Widevine downloads and updates from Google**.
3. Wait for an installed Widevine version to appear.
4. Quit Noventra completely, then reopen it.
5. At `chrome://settings/content/protectedContent`, allow sites to play protected content.
6. Reload the streaming page.

Allowing protected content does not install Widevine. Its Google download
consent is independent of other optional services. The Components page reports
when Widevine is missing and when a full restart is needed.

Public encrypted playback verifies the DRM engine, not acceptance by every paid
service. Netflix and Voyo authenticated playback remain unverified.
[Netflix's M7701-1003 guidance](https://help.netflix.com/en/node/27451/) notes that
Chromium browsers other than Chrome may be unsupported.

## Privacy and qualification

The inherited Helium fingerprinting protection, cookie controls, HTTPS
protection, Secure DNS, and optional-service consent remain available. This
release does not change their defaults or establish whole-browser privacy
superiority. Widevine remains optional.

The exact Linux candidate passed default-browser activation and restart checks,
five tab-memory checks including a rendered hover estimate, and 134 Settings
checks over sixteen routes at four sizes. Twenty-two media checks covered an
actual Widevine download, restart, encrypted decode, seeking, and revocation.
Real disposable package installation, AppRun launch, URL forwarding, repeated
installation, and uninstall also passed.

Tests used private Xvfb displays, owned profiles and XDG directories, and local
fixture pages. Native macOS, Windows, HiDPI, screen readers, and subscription
account playback were not qualified. Native fixture source is included for
review. The private display and network wrappers remain local prerequisites.
