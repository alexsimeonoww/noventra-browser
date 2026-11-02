// Copyright 2026 The Noventra Authors
#ifndef CHROME_BROWSER_UI_NOVENTRA_WINDOW_MIRROR_SERVICE_H_
#define CHROME_BROWSER_UI_NOVENTRA_WINDOW_MIRROR_SERVICE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/image/image_skia.h"
#include "url/gurl.h"

class BrowserWindowInterface;
class Profile;
namespace content {
class WebContents;
}

namespace noventra {

inline constexpr char kWindowMirroring[] = "noventra.browser.window_mirroring";
inline constexpr char kWindowMirrorState[] =
    "noventra.browser.window_mirror_state";
inline constexpr char kMirrorTabExtraData[] = "noventra.mirror_tab";
inline constexpr char kMirrorWindowExtraData[] = "noventra.mirror_window";

class WindowMirrorServiceFactory;

// UI-sequence catalog and ownership coordinator. Projection rows are not tabs:
// every logical ID resolves to exactly one real TabModel/WebContents. Mutating
// methods may invalidate projection rows; defer calls until input/drag cleanup.
class WindowMirrorService final : public KeyedService {
 public:
  using LogicalId = std::string;
  struct Row {
    LogicalId id;
    tabs::TabHandle handle;
    std::u16string title;
    GURL url;
    ui::ImageModel favicon;
    std::string space;
    int owner_window_id = 0;
    bool pinned = false;
    bool selected = false;
    bool local = false;
    std::optional<split_tabs::SplitTabId> split;
    std::optional<tab_groups::TabGroupId> group;
  };
  enum class CloseResult { kUseNormalClose, kRehomed, kBlocked };
  struct Snapshot {
    LogicalId id;
    std::string space;
    gfx::ImageSkia image;
  };

  static WindowMirrorService* GetForProfile(Profile* profile);
  static WindowMirrorService* GetForProfileIfExisting(Profile* profile);
  static bool IsEnabled(Profile* profile);
  static void EnsureFactoryBuilt();
  ~WindowMirrorService() override;

  bool IsEnabled() const;
  bool ObserveWindow(BrowserWindowInterface* browser);
  bool IsEnrolled(BrowserWindowInterface* browser) const;
  std::vector<Row> Rows(BrowserWindowInterface* browser) const;
  LogicalId Selected(BrowserWindowInterface* browser) const;
  std::string SelectedSpace(BrowserWindowInterface* browser) const;
  bool IsSelectedRemote(BrowserWindowInterface* browser) const;
  std::optional<Snapshot> GetSnapshot(BrowserWindowInterface* browser) const;

  // Changed notifications are coalesced; invalidation is synchronous and runs
  // BEFORE an actual tab is detached/deleted so views drop borrowed references.
  base::CallbackListSubscription RegisterChanged(base::RepeatingClosure cb);
  base::CallbackListSubscription RegisterWillDetach(
      base::RepeatingCallback<void(tabs::TabHandle)> cb);

  bool Select(BrowserWindowInterface* browser, const LogicalId& id);
  // Handle an existing catalog Space without creating a duplicate local NTP.
  // False means the normal Space switch should create its first actual tab.
  bool SelectSpace(BrowserWindowInterface* browser, const std::string& space);
  bool Close(BrowserWindowInterface* browser, const LogicalId& id);
  bool SetPinned(BrowserWindowInterface* browser,
                 const LogicalId& id,
                 bool pinned);
  // Empty before_id means append within this Space/pin section. Whole split or
  // group blocks move together; actual documents are never recreated.
  bool MoveLogical(BrowserWindowInterface* browser,
                   const LogicalId& id,
                   const LogicalId& before_id);
  bool DropToSplit(BrowserWindowInterface* browser,
                   const LogicalId& dragged,
                   const LogicalId& target,
                   split_tabs::SplitTabLayout layout);
  // Call with activation changes; claim on activation, one-shot capture on
  // deactivation. Failed/stale captures leave an empty image with honest UI.
  void OnActivated(BrowserWindowInterface* browser, bool active);
  // Mirror shells may have no actual owned tabs while showing a remote row.
  bool KeepEmptyWindow(BrowserWindowInterface* browser) const;
  // Call BEFORE CloseAllTabs. kBlocked must cancel/defer the close attempt,
  // never fall through and destroy pages still visible in peer projections.
  // |quitting| is true for coordinated application shutdown.
  CloseResult PrepareWindowClose(BrowserWindowInterface* browser,
                                 bool quitting);
  void WindowCloseCancelled(BrowserWindowInterface* browser);
  // Normally invoked automatically after this Profile's native session restore.
  // Recreates only validated missing empty views whose actual page ID exists.
  void RestoreMissingViews();

  // Session data contains IDs/order/selection only, never URL copies. Restore
  // tab metadata before insertion, window metadata after native owners exist.
  static void PopulateTabExtraData(
      const content::WebContents* contents,
      std::map<std::string, std::string>* extra_data);
  static void RestoreTabExtraData(
      content::WebContents* contents,
      const std::map<std::string, std::string>& extra_data);
  void PopulateWindowExtraData(
      BrowserWindowInterface* browser,
      std::map<std::string, std::string>* extra_data) const;
  void RestoreWindowExtraData(
      BrowserWindowInterface* browser,
      const std::map<std::string, std::string>& extra_data);

  void Shutdown() override;

 private:
  friend class WindowMirrorServiceFactory;
  explicit WindowMirrorService(Profile* profile);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noventra
#endif  // CHROME_BROWSER_UI_NOVENTRA_WINDOW_MIRROR_SERVICE_H_
