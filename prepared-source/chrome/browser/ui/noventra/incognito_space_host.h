// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_NOVENTRA_INCOGNITO_SPACE_HOST_H_
#define CHROME_BROWSER_UI_NOVENTRA_INCOGNITO_SPACE_HOST_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"

class BrowserNativeWidget;
class BrowserView;
class BrowserWidget;
class BrowserWindowInterface;
class Profile;
struct BrowserWindowCreateParams;

namespace content {
class WebContents;
}

namespace noventra {

inline constexpr char kProfileSpaceHostKey[] = "noventra.space_host";
inline constexpr char kProfileSpaceOpenKey[] = "noventra.open_spaces";
inline constexpr char kProfileSpaceSelectedKey[] = "noventra.selected_space";

// Complete native window metadata is shared by each regular presentation in
// one physical host. Private membership and selection never enter sessions.
void PopulateProfileSpaceWindowExtraData(
    BrowserWindowInterface* browser,
    std::map<std::string, std::string>* extra_data);
void RestoreProfileSpaceWindowIdentity(
    BrowserWindowInterface* browser,
    const std::map<std::string, std::string>& extra_data,
    int32_t restore_id);
BrowserWindowInterface* CreateRestoredProfileSpaceBrowser(
    BrowserWindowCreateParams params,
    const std::map<std::string, std::string>& extra_data,
    int32_t restore_id,
    BrowserWindowInterface* preferred_host = nullptr);

// An Incognito Space owns a complete off-the-record Browser and tab model.
// Its logical top-level widget shares the regular window's desktop Aura root;
// it owns focus/IME state without creating a second OS window.
// These identities and labels exist only while that private Browser is alive.
struct IncognitoSpaceEntry {
  std::string id;
  std::u16string name;
};

BrowserWindowInterface* CreateIncognitoSpace(BrowserWindowInterface* source,
                                             bool add_initial_tab = true);
BrowserWindowInterface* CreateIncognitoSpaceForProfile(
    Profile* profile,
    bool add_initial_tab = true);
// Profile loading is asynchronous. A later selection or close invalidates an
// earlier request. Returns false only when profile-backed Spaces are disabled.
bool SelectProfileSpace(BrowserWindowInterface* source, const std::string& id);
// Loads and restores a regular target. Default selection stays unchanged.
// With select=true, a later selection invalidates the pending activation.
// The callback must use the Browser synchronously; shutdown returns nullptr.
void AcquireProfileSpaceBrowser(
    BrowserWindowInterface* source,
    const std::string& id,
    base::OnceCallback<void(BrowserWindowInterface*)> callback,
    bool select = false);
// Used by native session restore after acquiring the target Profile. Creates
// an attached Browser without selecting it, adding tabs, or changing restore
// flags. The caller retains the normal native Profile keepalive until return.
BrowserWindowInterface* CreateProfileSpaceBrowser(
    BrowserWindowInterface* physical_host,
    BrowserWindowCreateParams params);
// The host and selected-presentation helpers serve both regular and private
// child Browsers. Private inventory and IncognitoSpaceId remain private-only.
BrowserWindowInterface* IncognitoSpaceHost(BrowserWindowInterface* browser);
bool IsIncognitoSpaceHostClosing(BrowserWindowInterface* browser);
BrowserWindowInterface* SelectedIncognitoBrowser(
    BrowserWindowInterface* browser);
BrowserWindowInterface* IncognitoSpaceTarget(BrowserWindowInterface* browser,
                                             const std::string& id);
bool IncognitoPresentationIsSelected(BrowserWindowInterface* browser);
// Selection changes logical presentation, raising the physical window before
// activating the selected logical widget and its independent FocusManager.
void ShowIncognitoPresentation(BrowserWindowInterface* browser);
bool DispatchIncognitoHostActivation(BrowserWindowInterface* browser,
                                     bool active);
// JS modal dismissal can leave a root-sibling hosted widget inactive while
// the OS host stays active. Restore only the same selected live presentation,
// after the dialog teardown, and never activate an inactive physical window.
void RestoreIncognitoFocusAfterModal(content::WebContents* contents);
std::string IncognitoSpaceId(BrowserWindowInterface* browser);
std::vector<IncognitoSpaceEntry> IncognitoSpaces(
    BrowserWindowInterface* browser);
// Selects through the ordinary Space command without moving WebContents.
bool SelectIncognitoSpace(BrowserWindowInterface* browser,
                          const std::string& id);
base::CallbackListSubscription ObserveIncognitoSpaces(
    base::RepeatingClosure callback);

// Browser-widget integration. The native factory returns null outside a
// supported, explicitly scoped hosted-space creation.
BrowserNativeWidget* MaybeCreateIncognitoNativeWidget(BrowserWidget* widget,
                                                      BrowserView* view);
void LayoutIncognitoSpaces(BrowserView* host_view);
bool PrepareIncognitoHostClose(BrowserWindowInterface* browser);
bool KeepEmptyIncognitoHost(BrowserWindowInterface* browser);
void IncognitoBrowserViewDestroying(BrowserWindowInterface* browser);
void IncognitoWindowCloseCancelled(BrowserWindowInterface* browser);

}  // namespace noventra
#endif
