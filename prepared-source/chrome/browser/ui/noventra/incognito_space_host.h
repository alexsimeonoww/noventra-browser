// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_NOVENTRA_INCOGNITO_SPACE_HOST_H_
#define CHROME_BROWSER_UI_NOVENTRA_INCOGNITO_SPACE_HOST_H_

#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"

class BrowserNativeWidget;
class BrowserView;
class BrowserWidget;
class BrowserWindowInterface;
class Profile;

namespace content {
class WebContents;
}

namespace noventra {

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
BrowserWindowInterface* IncognitoSpaceHost(BrowserWindowInterface* browser);
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
// JS modal dismissal can leave a root-sibling private widget inactive while
// the OS host stays active. Restore only the same selected live presentation,
// after the dialog teardown, and never activate an inactive physical window.
void RestoreIncognitoFocusAfterModal(content::WebContents* contents);
std::string IncognitoSpaceId(BrowserWindowInterface* browser);
std::vector<IncognitoSpaceEntry> IncognitoSpaces(
    BrowserWindowInterface* browser);
// A private identity selects its own Browser. A regular identity returns to
// the regular model without moving or reparenting any tab across profiles.
bool SelectIncognitoSpace(BrowserWindowInterface* browser,
                          const std::string& id);
base::CallbackListSubscription ObserveIncognitoSpaces(
    base::RepeatingClosure callback);

// Browser-widget integration. The native factory returns null outside a
// supported, explicitly scoped private-space creation.
BrowserNativeWidget* MaybeCreateIncognitoNativeWidget(BrowserWidget* widget,
                                                      BrowserView* view);
void LayoutIncognitoSpaces(BrowserView* host_view);
bool PrepareIncognitoHostClose(BrowserWindowInterface* browser);
bool KeepEmptyIncognitoHost(BrowserWindowInterface* browser);
void IncognitoBrowserViewDestroying(BrowserWindowInterface* browser);
void IncognitoWindowCloseCancelled(BrowserWindowInterface* browser);

}  // namespace noventra
#endif
