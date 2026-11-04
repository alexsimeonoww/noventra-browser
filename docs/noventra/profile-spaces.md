# Independent browser profiles for Spaces

Regular Spaces on Linux use separate persistent Chromium profiles. Switching
Spaces keeps their loaded documents in the same physical window. The shared
Space names, order, appearance and archive state remain in Personal's catalog.
Profile settings, including the browser layout, belong to each regular Space.

| Data | Regular Spaces | Incognito |
| --- | --- | --- |
| Cookies, website logins and site storage | Separate per Space | Temporary private storage |
| History and history suggestions | Separate per Space | Suggestions can use its parent Space; ordinary private visits are not saved |
| Bookmarks and saved passwords | Separate per Space | Reads its parent Space's library; explicit bookmark saves persist there |
| Download list | Separate per Space | Private list is temporary; downloaded files remain on disk |
| Extensions and extension storage | Separate per Space | Chromium's existing Incognito extension permissions apply |
| Website permissions and profile settings | Separate per Space | Chromium's existing private-profile behavior applies |

Incognito opened from Work belongs to Work. Its shortcut stays visible while
switching between regular Spaces. Closing its last tab removes the temporary
shortcut and returns to its regular parent. A Space's name or color never changes
its storage identity.

## Existing installations

Migration runs before the selected regular profiles open. Personal keeps the
original profile, including the history, bookmarks, passwords and download lists
that older versions shared. Those records lack reliable Space ownership, so they
are not copied into every new profile.

Each existing non-Personal Space keeps its stable UUID. Its entire legacy website
storage partition is copied into its new profile at the same relative path.
Native session metadata preserves tab ownership, pins, groups and splits. A cold
copy of the original profile and a migration journal are retained for recovery.
The original website partition is preserved. A failed or conflicting migration
stops startup rather than silently opening an empty replacement profile.

New Spaces start with a fresh profile and default website partition. Profile
bindings are committed before the new profile can initialize. Native session
saving waits until any prepared restore input has been materialized.

Older named partitions did not store memory-only session cookies on disk. A cold
upgrade cannot recover cookies that the old runtime never wrote. Persistent
cookies and other retained website storage migrate. Newly created session
cookies and sessionStorage follow the new Space session-restoration policy.

Historical closed-tab records remain in Personal and the recovery copy. Restore
eligibility prevents foreign or mixed-profile entries from opening in Personal.
Replaying those old closed-tab records directly into their new owning profile is
not implemented. Newly closed tabs use their own profile's native restore log.

## Pins and splits

Pinned tiles measure 32 DIP in compact density and 36 DIP in comfortable density.
Saved shortcuts retain their favicon colors. Close appears on hover or keyboard
focus only while the page is loaded. Closing unloads the page and keeps its pin.
Clicking that saved pin opens it again.

A pinned tab can split with an ordinary tab in the same Space. Chromium split
collections have one pin state, so a split containing a pin keeps all its members
pinned. Separating the tabs keeps those members pinned. Group deletion still
requires the native confirmation; Cancel preserves the existing group and pages.
Cross-Space or cross-profile combinations remain rejected.

The full-profile host is enabled on Linux/Aura. macOS and Windows have not been
qualified for this host implementation.

## Native qualification

The Linux candidate passed 306 native checks across 13 runs and 17 normal browser
exits. Its binary SHA-256 is
`a6bf80d0ba4d53531f290429133a3aa74e77dd2e2cf50792406be04d261dceca`.
Its resources SHA-256 is
`8c64408b6f307e2b1dcfcea6feb9647a2d2a82781c2dde1c55fdd21f9bebff81`.

| Flow | Native checks |
| --- | ---: |
| Profile services, retained documents and Incognito parent on X11 and Wayland | 62 |
| Preview-6 migration, native session restore and a second restart | 47 |
| Sidebar width, three shortcuts and retained regular/private pages | 93 |
| Native mixed-pin split, rejoin and permanent tearoff | 25 |
| Direct closed-pin split picker in both tab layouts | 16 |
| Compact Wayland and comfortable X11 pin visuals and pointer Close | 16 |
| Work pin-to-ordinary split on Wayland | 10 |
| Two-client workspace sync, profile-owned layout durability and restart | 37 |

The complete browser linked successfully. Eight GN checks, the changed C++
compilation targets, ten migration tests and six restore-eligibility tests passed.
The exported patch replayed with zero fuzz and no offsets, and its prepared
source hashes match the compiled candidate. All 681 shared canonical source
files remained unchanged.

Native fixtures use disposable profiles, authenticated private displays and
local pages. The complete Chromium test suite, power-loss fault injection and
native macOS/Windows qualification were not run. Signed package and in-browser
update qualification is recorded separately for each release.
