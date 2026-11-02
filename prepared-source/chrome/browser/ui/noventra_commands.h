// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_NOVENTRA_COMMANDS_H_
#define CHROME_BROWSER_UI_NOVENTRA_COMMANDS_H_
// Project commands in the bounded native menu range, below 0xDFFF.
inline constexpr int IDC_NOVENTRA_ZEN = 57000;
inline constexpr int IDC_NOVENTRA_SIDEBAR = 57001;
inline constexpr int IDC_NOVENTRA_SIDEBAR_SIDE = 57002;
inline constexpr int IDC_NOVENTRA_CUSTOMIZE = 57003;
inline constexpr int IDC_NOVENTRA_SHORTCUTS = 57004;
inline constexpr int IDC_NOVENTRA_PREVIEW_LINK = 57005;
inline constexpr char kNoventraLinkPreview[] = "noventra.browser.link_preview";
inline constexpr char kNoventraLinkPreviewTrigger[] =
    "noventra.browser.link_preview_trigger";
inline constexpr char kNoventraLinkPreviewPinned[] =
    "noventra.browser.link_preview_pinned";
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
class BrowserWindowInterface;
class Profile;
inline constexpr int IDC_NOVENTRA_NEXT_SPACE = 57006;
inline constexpr int IDC_NOVENTRA_PREVIOUS_SPACE = 57007;
inline constexpr int IDC_NOVENTRA_MANAGE_SPACES = 57008;
// Ordered, visible space positions; identities remain storage-partition IDs.
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_0 = 57009;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_1 = 57010;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_2 = 57011;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_3 = 57012;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_4 = 57013;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_5 = 57014;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_6 = 57015;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_7 = 57016;
inline constexpr int IDC_NOVENTRA_SELECT_SPACE_8 = 57017;
inline constexpr int IDC_NOVENTRA_GRID_SPLIT = 57018;
namespace noventra {
// Visible navigation appends live temporary private Spaces to the saved order.
// include_archived returns persistent metadata ONLY, for Settings/export/routes;
// private identities must never be serialized. Personal has the empty ID.
std::vector<std::string> SpaceIds(BrowserWindowInterface* browser,
                                  bool include_archived = false);
// The selected logical Browser is authoritative for temporary private Spaces.
std::string CurrentSpaceId(BrowserWindowInterface* browser);
bool IsSpaceArchived(Profile* profile, const std::string& id);
bool CanManageSpaces(Profile* profile);
// Appearance is account-neutral display metadata. Invalid stored choices fall
// back to the default dot and theme color; updates require an existing space.
struct SpaceAppearance {
  std::string color = "default";
  std::string icon = "default";
  std::string theme = "browser";
};
// Window-local preset values. A null effective theme inherits the profile
// theme; switching Spaces never writes the profile ThemeService preferences.
struct SpaceTheme {
  std::string_view id;
  bool dark;
  bool glass;
  uint32_t seed;  // Zero uses the grayscale palette.
};
bool CanCustomizeSpaceTheme(BrowserWindowInterface* browser);
const SpaceTheme* GetSpaceTheme(BrowserWindowInterface* browser);
SpaceAppearance GetSpaceAppearance(Profile* profile, const std::string& id);
bool SetSpaceAppearance(BrowserWindowInterface* browser,
                        const std::string& id,
                        const std::string& color,
                        const std::string& icon,
                        const std::string& theme);
// Creation requires a new lowercase UUID; rename/move/archive require an
// existing space. None of these operations change a tab's account partition.
bool SetSpaceName(BrowserWindowInterface* browser,
                  const std::string& id,
                  const std::string& name,
                  bool create);
bool MoveSpace(BrowserWindowInterface* browser,
               const std::string& id,
               const std::string& target,
               bool after);
bool SetSpaceArchived(BrowserWindowInterface* browser,
                      const std::string& id,
                      bool archived);
void SwitchSpace(BrowserWindowInterface* browser, const std::string& id);
void CycleSpace(BrowserWindowInterface* browser, int direction);
}
#endif
