// Copyright 2026 The Noventra Authors
#include "chrome/browser/ui/noventra/window_mirror_service.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

#include "base/auto_reset.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/sequence_checker.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/noventra_account_spaces.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_manager_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/noventra/pinned_tab_state.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/split_tab_data.h"
#include "components/tabs/public/tab_group.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/base/base_window.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace noventra {
namespace {

const char kMirrorTabStateKey = 0;
constexpr size_t kMaximumMetadataBytes = 1024;
constexpr size_t kMaximumSnapshots = 4;
constexpr size_t kMaximumRestoredViews = 16;
constexpr int kMaximumSnapshotWidth = 1024;
constexpr int kMaximumSnapshotHeight = 768;

bool Eligible(Profile* profile, BrowserWindowInterface* browser) {
  return profile && profile->IsRegularProfile() && !profile->IsOffTheRecord() &&
         browser && browser->GetProfile() == profile &&
         browser->GetType() == BrowserWindowInterface::TYPE_NORMAL &&
         !browser->IsDeleteScheduled();
}

bool ValidId(const std::string& id) {
  return id.size() == 36 && base::Uuid::ParseLowercase(id).is_valid();
}

class MirrorTabState final : public base::SupportsUserData::Data,
                             public content::WebContentsObserver {
 public:
  explicit MirrorTabState(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame()) {
      ++navigation_epoch;
    }
  }

  void AboutToBeDiscarded(content::WebContents* replacement) override {
    auto copy = std::make_unique<MirrorTabState>(replacement);
    copy->id = id;
    copy->order = order;
    copy->navigation_epoch = navigation_epoch + 1;
    replacement->SetUserData(&kMirrorTabStateKey, std::move(copy));
  }

  std::string id;
  int order = -1;
  uint64_t navigation_epoch = 0;
  std::string persisted;
  int persisted_window = 0;
};

const MirrorTabState* Metadata(const content::WebContents* contents) {
  return contents ? static_cast<const MirrorTabState*>(
                        contents->GetUserData(&kMirrorTabStateKey))
                  : nullptr;
}

MirrorTabState* EnsureMetadata(content::WebContents* contents) {
  auto* state =
      static_cast<MirrorTabState*>(contents->GetUserData(&kMirrorTabStateKey));
  if (!state) {
    auto created = std::make_unique<MirrorTabState>(contents);
    state = created.get();
    contents->SetUserData(&kMirrorTabStateKey, std::move(created));
  }
  if (!ValidId(state->id)) {
    state->id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  return state;
}

std::string Serialize(const MirrorTabState& state) {
  auto json = base::WriteJson(base::DictValue()
                                  .Set("version", 1)
                                  .Set("id", state.id)
                                  .Set("order", state.order));
  return json.value_or(std::string());
}

base::DictValue BoundedViewDescriptors(const base::DictValue& stored) {
  base::DictValue result;
  size_t inspected = 0;
  for (const auto [id, value] : stored) {
    if (++inspected > kMaximumRestoredViews * 4 ||
        result.size() >= kMaximumRestoredViews) {
      break;
    }
    const auto* descriptor = value.GetIfDict();
    if (!ValidId(id) || !descriptor || descriptor->FindInt("version") != 1) {
      continue;
    }
    const auto* selected = descriptor->FindString("selected");
    const auto* space = descriptor->FindString("space");
    const auto x = descriptor->FindInt("x");
    const auto y = descriptor->FindInt("y");
    const auto width = descriptor->FindInt("width");
    const auto height = descriptor->FindInt("height");
    if (!selected || !ValidId(*selected) || !space ||
        (!space->empty() && !ValidId(*space)) || !x || !y || !width ||
        !height || *x < -1000000 || *x > 1000000 || *y < -1000000 ||
        *y > 1000000 || *width < 100 || *width > 16384 || *height < 100 ||
        *height > 16384) {
      continue;
    }
    // Copy known bounded scalar fields only; unknown nested preference data
    // never enters the retained startup state.
    result.Set(id, base::DictValue()
                       .Set("version", 1)
                       .Set("selected", *selected)
                       .Set("space", *space)
                       .Set("x", *x)
                       .Set("y", *y)
                       .Set("width", *width)
                       .Set("height", *height));
  }
  return result;
}

void PersistTab(content::WebContents* contents) {
  auto* state = const_cast<MirrorTabState*>(Metadata(contents));
  auto* helper = sessions::SessionTabHelper::FromWebContents(contents);
  if (!state || !helper || !helper->window_id().is_valid()) {
    return;
  }
  const std::string serialized = Serialize(*state);
  if (serialized.empty() ||
      (state->persisted == serialized &&
       state->persisted_window == helper->window_id().id())) {
    return;
  }
  auto* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  if (auto* service = SessionServiceFactory::GetForProfileIfExisting(profile)) {
    service->AddTabExtraData(helper->window_id(), helper->session_id(),
                             kMirrorTabExtraData, serialized);
    state->persisted = serialized;
    state->persisted_window = helper->window_id().id();
  }
}

}  // namespace

class WindowMirrorService::Impl final : public BrowserCollectionObserver,
                                        public TabStripModelObserver {
 public:
  explicit Impl(Profile* profile) : profile_(profile) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (profile_->GetPrefs()->FindPreference(kWindowMirrorState)) {
      startup_views_ = BoundedViewDescriptors(
          profile_->GetPrefs()->GetDict(kWindowMirrorState));
    }
    restored_subscription_ =
        SessionRestore::RegisterOnSessionRestoredCallback(base::BindRepeating(
            &Impl::OnSessionRestored, weak_factory_.GetWeakPtr()));
    prefs_.Init(profile_->GetPrefs());
    prefs_.Add(kWindowMirroring,
               base::BindRepeating(&Impl::OnPreferenceChanged,
                                   weak_factory_.GetWeakPtr()));
    for (const char* pref : {kSpacesPref, kArchivedSpacesPref}) {
      if (profile_->GetPrefs()->FindPreference(pref)) {
        prefs_.Add(pref, base::BindRepeating(&Impl::OnSpacePreferencesChanged,
                                             weak_factory_.GetWeakPtr()));
      }
    }
    collection_.Observe(ProfileBrowserCollection::GetForProfile(profile_));
    OnPreferenceChanged();
  }

  ~Impl() override { Shutdown(); }

  struct WindowState {
    raw_ptr<BrowserWindowInterface> browser = nullptr;
    raw_ptr<TabStripModel> model = nullptr;
    std::string view_id;
    std::string persisted_window;
    LogicalId selected;
    std::string space;
    bool closing = false;
    uint64_t activation = 0;
    uint64_t snapshot_epoch = 0;
    std::optional<Snapshot> snapshot;
  };
  struct Entry {
    Row row;
    int order = 0;
  };
  struct Recovery {
    std::unique_ptr<DetachedTabCollection> collection;
    std::unique_ptr<tabs::TabModel> tab;
    bool group = false;
    bool pinned = false;
  };

  bool Enabled() const {
    return !shutdown_ && WindowMirrorService::IsEnabled(profile_);
  }

  WindowState* State(BrowserWindowInterface* browser) {
    if (!browser) {
      return nullptr;
    }
    auto it = windows_.find(browser->GetSessionID().id());
    return it != windows_.end() && it->second.browser == browser ? &it->second
                                                                 : nullptr;
  }

  const WindowState* State(BrowserWindowInterface* browser) const {
    return const_cast<Impl*>(this)->State(browser);
  }

  BrowserWindowInterface* BrowserFor(int id) const {
    auto it = windows_.find(id);
    return it != windows_.end() ? it->second.browser.get() : nullptr;
  }

  bool Enrolled(BrowserWindowInterface* browser) const {
    return Enabled() && Eligible(profile_, browser) && State(browser);
  }

  bool SpaceExists(const std::string& space) const {
    return space.empty() ||
           (ValidId(space) &&
            profile_->GetPrefs()->GetDict(kSpacesPref).contains(space) &&
            !IsSpaceArchived(profile_, space));
  }

  bool ObserveWindow(BrowserWindowInterface* browser) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!Enabled() || !Eligible(profile_, browser)) {
      return false;
    }
    if (State(browser)) {
      return true;
    }
    WindowState state;
    state.browser = browser;
    state.model = browser->GetTabStripModel();
    state.view_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    // A new window inherits the most recently active enrolled view's Space and
    // selected logical page. It does not create another actual page.
    const WindowState* previous = nullptr;
    for (const auto& [id, candidate] : windows_) {
      if (!candidate.closing &&
          (!previous || candidate.activation > previous->activation)) {
        previous = &candidate;
      }
    }
    if (previous) {
      state.space = previous->space;
      state.selected = previous->selected;
    } else if (auto* contents =
                   browser->GetTabStripModel()->GetActiveWebContents()) {
      state.space = SpaceId(contents);
      state.selected = EnsureMetadata(contents)->id;
    }
    windows_.emplace(browser->GetSessionID().id(), std::move(state));
    browser->GetTabStripModel()->AddObserver(this);
    RecoverDetached();
    Reconcile();
    return true;
  }

  std::vector<Row> Rows(BrowserWindowInterface* browser) const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    std::vector<Row> result;
    const auto* window = State(browser);
    if (!Enrolled(browser) || !window || !SpaceExists(window->space)) {
      return result;
    }
    for (const auto& [id, entry] : entries_) {
      if (entry.row.space != window->space || !entry.row.handle.Get()) {
        continue;
      }
      Row row = entry.row;
      row.selected = window->selected == id;
      row.local = row.owner_window_id == browser->GetSessionID().id();
      result.push_back(std::move(row));
    }
    std::ranges::stable_sort(result, [this](const Row& a, const Row& b) {
      if (a.pinned != b.pinned) {
        return a.pinned > b.pinned;
      }
      const int left = entries_.at(a.id).order;
      const int right = entries_.at(b.id).order;
      return left == right ? a.id < b.id : left < right;
    });
    return result;
  }

  void ScheduleEmptyShellCleanup() {
    if (shutdown_ || empty_cleanup_pending_) {
      return;
    }
    empty_cleanup_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&Impl::CloseEmptyShells, weak_factory_.GetWeakPtr()));
  }

  void CloseEmptyShells() {
    empty_cleanup_pending_ = false;
    if (shutdown_ || mutation_ || !recovery_.empty() ||
        (Enabled() && !entries_.empty())) {
      return;
    }
    // A newly constructed browser may not yet have inserted its initial tab.
    // Check actual ownership again after the posted task, not only the cache.
    if (Enabled() && std::ranges::any_of(windows_, [](const auto& pair) {
          return !pair.second.model->empty();
        })) {
      return;
    }
    std::vector<int> empty;
    for (const auto& [id, window] : windows_) {
      if (!window.closing && window.model->empty()) {
        empty.push_back(id);
      }
    }
    for (int id : empty) {
      if (auto* browser = BrowserFor(id);
          Eligible(profile_, browser) && browser->GetTabStripModel()->empty() &&
          browser->GetWindow()) {
        browser->GetWindow()->Close();
      }
    }
  }

  void ScheduleChanged() {
    if (shutdown_ || notify_pending_) {
      return;
    }
    notify_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Impl::Notify, weak_factory_.GetWeakPtr()));
  }

  void Notify() {
    notify_pending_ = false;
    if (!shutdown_) {
      SaveViewDescriptors();
      changed_.Notify();
    }
  }

  void ScheduleReconcile() {
    if (shutdown_ || reconcile_pending_) {
      return;
    }
    reconcile_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&Impl::ReconcilePosted, weak_factory_.GetWeakPtr()));
  }

  void ReconcilePosted() {
    reconcile_pending_ = false;
    Reconcile();
  }

  void Reconcile() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!Enabled() || mutation_ || reconciling_) {
      return;
    }
    base::AutoReset<bool> guard(&reconciling_, true);
    std::map<LogicalId, Entry> next;
    int next_order = 0;
    for (const auto& [id, entry] : entries_) {
      next_order = std::max(next_order, entry.order + 1);
    }
    for (auto& [window_id, window] : windows_) {
      if (!Eligible(profile_, window.browser)) {
        continue;
      }
      auto* model = window.browser->GetTabStripModel();
      for (tabs::TabInterface* tab : *model) {
        auto* contents = tab->GetContents();
        if (!contents || contents->IsBeingDestroyed() ||
            contents->GetBrowserContext() != profile_) {
          continue;
        }
        auto* metadata = EnsureMetadata(contents);
        // Restore/clone mistakes must not make two real documents one logical
        // tab. A conflict gets a fresh ID, never closes or merges either page.
        if (next.contains(metadata->id) &&
            next.at(metadata->id).row.handle != tab->GetHandle()) {
          metadata->id = base::Uuid::GenerateRandomV4().AsLowercaseString();
          metadata->order = -1;
        }
        Entry entry;
        auto old = entries_.find(metadata->id);
        entry.order = old != entries_.end()  ? old->second.order
                      : metadata->order >= 0 ? metadata->order
                                             : next_order++;
        metadata->order = entry.order;
        const auto data = tabs::TabData::FromTabInterface(tab);
        entry.row.id = metadata->id;
        entry.row.handle = tab->GetHandle();
        entry.row.title = PinnedTabDisplayTitle(contents, data.title);
        entry.row.url = data.visible_url;
        entry.row.favicon = data.favicon;
        entry.row.space = SpaceId(contents);
        entry.row.owner_window_id = window_id;
        entry.row.pinned = tab->IsPinned();
        entry.row.split = tab->GetSplit();
        entry.row.group = tab->GetGroup();
        next.emplace(metadata->id, std::move(entry));
        PersistTab(contents);
      }
    }
    entries_ = std::move(next);
    if (entries_.empty()) {
      ScheduleEmptyShellCleanup();
    }
    for (auto& [id, window] : windows_) {
      auto selected = entries_.find(window.selected);
      if (selected != entries_.end() &&
          selected->second.row.space == window.space &&
          SpaceExists(window.space)) {
        continue;
      }
      window.snapshot.reset();
      ++window.snapshot_epoch;
      if (!SpaceExists(window.space)) {
        window.space.clear();
      }
      window.selected.clear();
      if (auto* active = window.browser->GetTabStripModel()->GetActiveTab()) {
        const auto* metadata = Metadata(active->GetContents());
        if (metadata && SpaceId(active->GetContents()) == window.space &&
            entries_.contains(metadata->id)) {
          window.selected = metadata->id;
        }
      }
      if (window.selected.empty()) {
        auto rows = Rows(window.browser);
        if (!rows.empty()) {
          window.selected = rows.front().id;
        }
      }
    }
    ScheduleChanged();
  }

  tabs::TabInterface* Resolve(const LogicalId& id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.row.handle.Get();
  }

  bool Allowed(BrowserWindowInterface* browser, const LogicalId& id) const {
    if (!Enrolled(browser) || mutation_) {
      return false;
    }
    const auto* window = State(browser);
    auto* tab = Resolve(id);
    return window && !window->closing && SpaceExists(window->space) && tab &&
           tab->GetContents()->GetBrowserContext() == profile_ &&
           SpaceId(tab->GetContents()) == window->space &&
           Enrolled(tab->GetBrowserWindowInterface());
  }

  std::vector<tabs::TabHandle> Block(tabs::TabInterface* tab) const {
    std::vector<tabs::TabHandle> result;
    if (!tab || !Enrolled(tab->GetBrowserWindowInterface())) {
      return result;
    }
    auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
    if (auto group = tab->GetGroup()) {
      if (!model->group_model() ||
          !model->group_model()->ContainsTabGroup(*group)) {
        return result;
      }
      const gfx::Range indices =
          model->group_model()->GetTabGroup(*group)->ListTabs();
      for (size_t i = indices.start(); i < indices.end(); ++i) {
        result.push_back(model->GetTabAtIndex(i)->GetHandle());
      }
    } else if (auto split = tab->GetSplit()) {
      if (!model->ListSplits().contains(*split)) {
        return result;
      }
      for (auto* member : model->GetSplitData(*split)->ListTabs()) {
        result.push_back(member->GetHandle());
      }
    } else {
      result.push_back(tab->GetHandle());
    }
    return result;
  }

  bool MoveBlock(const LogicalId& id,
                 BrowserWindowInterface* destination,
                 bool activate) {
    auto* tab = Resolve(id);
    if (!tab || !Enrolled(destination)) {
      return false;
    }
    auto* source = tab->GetBrowserWindowInterface();
    if (!Enrolled(source)) {
      return false;
    }
    auto* from = source->GetTabStripModel();
    auto* to = destination->GetTabStripModel();
    if (from == to) {
      if (activate) {
        to->ActivateTabAt(to->GetIndexOfTab(tab));
      }
      return true;
    }
    const auto handles = Block(tab);
    const std::string space = SpaceId(tab->GetContents());
    const bool pinned = tab->IsPinned();
    const auto group = tab->GetGroup();
    const auto split = tab->GetSplit();
    if (handles.empty() ||
        (group &&
         (!to->group_model() || to->group_model()->ContainsTabGroup(*group)))) {
      return false;
    }
    const auto existing_splits = to->ListSplits();
    for (auto handle : handles) {
      auto* member = handle.Get();
      if (!member || member->GetBrowserWindowInterface() != source ||
          member->GetContents()->GetBrowserContext() != profile_ ||
          SpaceId(member->GetContents()) != space ||
          member->GetContents()->IsBeingDestroyed() ||
          !member->CanShowModalUI() ||
          (member->GetSplit() &&
           existing_splits.contains(*member->GetSplit()))) {
        return false;
      }
    }
    const int source_id = source->GetSessionID().id();
    const int destination_id = destination->GetSessionID().id();
    const tabs::TabHandle selected_handle = tab->GetHandle();
    if (auto* source_state = State(source)) {
      for (auto handle : handles) {
        auto* member = handle.Get();
        if (member && EnsureMetadata(member->GetContents())->id ==
                          source_state->selected) {
          Capture(source, source_state->selected);
          break;
        }
      }
    }
    bool moved = false;
    {
      base::AutoReset<bool> transaction(&mutation_, true);
      // No user callbacks or posted actions run between native detach/insert;
      // pre-removal listeners only invalidate borrowed projection references.
      Recovery detached;
      detached.group = group.has_value();
      detached.pinned = pinned;
      if (group) {
        detached.collection = from->DetachTabGroupForInsertion(*group);
      } else if (split) {
        detached.collection = from->DetachSplitTabForInsertion(*split);
      } else {
        const int index = from->GetIndexOfTab(tab);
        if (!from->ContainsIndex(index)) {
          return false;
        }
        detached.tab = from->DetachTabAtForInsertion(index);
      }
      auto* destination_now = BrowserFor(destination_id);
      auto* rollback = BrowserFor(source_id);
      if (Eligible(profile_, destination_now)) {
        InsertRecovered(destination_now, std::move(detached));
      } else if (Eligible(profile_, rollback)) {
        InsertRecovered(rollback, std::move(detached));
      } else {
        // The required pre-close hook blocks reentrant teardown during this
        // transaction. Retain ownership defensively if a lower-level forced
        // teardown bypasses that hook; never silently destroy a live document.
        recovery_.push_back(std::move(detached));
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&Impl::RecoverDetached, weak_factory_.GetWeakPtr()));
      }
      auto* target = BrowserFor(destination_id);
      auto* selected = selected_handle.Get();
      if (target && selected &&
          selected->GetBrowserWindowInterface() == target) {
        moved = true;
        if (activate) {
          const int index = target->GetTabStripModel()->GetIndexOfTab(selected);
          if (target->GetTabStripModel()->ContainsIndex(index)) {
            target->GetTabStripModel()->ActivateTabAt(index);
          }
        }
      }
    }
    Reconcile();
    return moved;
  }

  void InsertRecovered(BrowserWindowInterface* browser, Recovery owned) {
    auto* model = browser->GetTabStripModel();
    if (owned.collection) {
      if (owned.group) {
        model->InsertDetachedTabGroupAt(std::move(owned.collection),
                                        model->count());
      } else {
        model->InsertDetachedSplitTabAt(std::move(owned.collection),
                                        model->count(), owned.pinned);
      }
    } else if (owned.tab) {
      model->InsertDetachedTabAt(model->count(), std::move(owned.tab),
                                 owned.pinned ? AddTabTypes::ADD_PINNED : 0);
    }
  }

  void RecoverDetached() {
    if (shutdown_ || mutation_ || recovery_.empty()) {
      return;
    }
    for (auto& [id, window] : windows_) {
      if (!window.closing && Eligible(profile_, window.browser)) {
        {
          base::AutoReset<bool> transaction(&mutation_, true);
          auto owned = std::move(recovery_);
          recovery_.clear();
          for (auto& detached : owned) {
            InsertRecovered(window.browser, std::move(detached));
          }
        }
        Reconcile();
        return;
      }
    }
  }

  bool Select(BrowserWindowInterface* browser, const LogicalId& id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!Allowed(browser, id)) {
      return false;
    }
    const int window_id = browser->GetSessionID().id();
    auto* state = State(browser);
    const LogicalId previous = state->selected;
    if (previous != id) {
      state->snapshot.reset();
      ++state->snapshot_epoch;
    }
    state->selected = id;
    const bool moved = MoveBlock(id, browser, true);
    if (!moved) {
      if (auto* live = BrowserFor(window_id)) {
        State(live)->selected = previous;
      }
    }
    ScheduleChanged();
    return moved;
  }

  bool SelectSpace(BrowserWindowInterface* browser, const std::string& space) {
    if (!Enrolled(browser) || mutation_ || !SpaceExists(space)) {
      return false;
    }
    auto* state = State(browser);
    if (state->space != space) {
      state->snapshot.reset();
      ++state->snapshot_epoch;
      state->space = space;
      state->selected.clear();
    }
    auto rows = Rows(browser);
    if (rows.empty()) {
      ScheduleChanged();
      return false;
    }
    const auto selected =
        std::ranges::find_if(rows, [](const Row& row) { return row.selected; });
    return Select(browser,
                  selected == rows.end() ? rows.front().id : selected->id);
  }

  bool Close(BrowserWindowInterface* browser, const LogicalId& id) {
    if (!Allowed(browser, id)) {
      return false;
    }
    auto* tab = Resolve(id);
    auto* owner = tab->GetBrowserWindowInterface();
    auto* model = owner->GetTabStripModel();
    if (!model->IsTabClosable(tab)) {
      return false;
    }
    if (tab->IsPinned()) {
      // Claim first so the standard pinned-tab confirmation toast appears in
      // the requesting window. The native command sees the actual active tab.
      const int requesting_id = browser->GetSessionID().id();
      if (!Select(browser, id)) {
        return false;
      }
      if (auto* requesting = BrowserFor(requesting_id)) {
        chrome::CloseTab(requesting);
        return true;
      }
      return false;
    }
    // Keep ordinary background-close and beforeunload behavior. The actual
    // owner hosts any native confirmation; no placeholder is destroyed.
    model->CloseWebContents(tab->GetContents(),
                            CLOSE_USER_GESTURE | CLOSE_CREATE_HISTORICAL_TAB);
    return true;
  }

  bool SetPinned(BrowserWindowInterface* browser,
                 const LogicalId& id,
                 bool pinned) {
    if (!Allowed(browser, id)) {
      return false;
    }
    auto* tab = Resolve(id);
    auto* model = tab->GetBrowserWindowInterface()->GetTabStripModel();
    const int index = model->GetIndexOfTab(tab);
    if (!model->ContainsIndex(index)) {
      return false;
    }
    model->SetTabPinned(index, pinned);
    Reconcile();
    return true;
  }

  bool MoveLogical(BrowserWindowInterface* browser,
                   const LogicalId& id,
                   const LogicalId& before_id) {
    if (!Allowed(browser, id) || id == before_id ||
        (!before_id.empty() && !Allowed(browser, before_id))) {
      return false;
    }
    auto* source = Resolve(id);
    auto* target = before_id.empty() ? nullptr : Resolve(before_id);
    if (target && source->IsPinned() != target->IsPinned()) {
      return false;
    }
    LogicalId before = before_id;
    if (target) {
      const auto target_block = Block(target);
      if (target_block.empty() || !target_block.front().Get()) {
        return false;
      }
      before = EnsureMetadata(target_block.front().Get()->GetContents())->id;
    }
    const auto block = Block(source);
    std::set<LogicalId> ids;
    std::vector<LogicalId> ordered;
    for (auto handle : block) {
      auto* tab = handle.Get();
      if (!tab || SpaceId(tab->GetContents()) != State(browser)->space ||
          tab->IsPinned() != source->IsPinned()) {
        return false;
      }
      const auto* state = Metadata(tab->GetContents());
      if (!state || !entries_.contains(state->id)) {
        return false;
      }
      ids.insert(state->id);
    }
    if (ids.empty() || ids.contains(before)) {
      return false;
    }
    for (const auto& row : Rows(browser)) {
      if (!ids.contains(row.id) && row.pinned == source->IsPinned()) {
        ordered.push_back(row.id);
      }
    }
    auto position = before_id.empty() ? ordered.end()
                                      : std::ranges::find(ordered, before_id);
    std::vector<LogicalId> moving;
    for (auto handle : block) {
      moving.push_back(Metadata(handle.Get()->GetContents())->id);
    }
    ordered.insert(position, moving.begin(), moving.end());
    int order = 0;
    for (const auto& logical : ordered) {
      auto& entry = entries_.at(logical);
      entry.order = order++;
      if (auto* tab = entry.row.handle.Get()) {
        auto* state = EnsureMetadata(tab->GetContents());
        state->order = entry.order;
        PersistTab(tab->GetContents());
      }
    }
    ScheduleChanged();
    return true;
  }

  bool DropToSplit(BrowserWindowInterface* browser,
                   const LogicalId& dragged,
                   const LogicalId& target,
                   split_tabs::SplitTabLayout layout) {
    if (dragged == target || !Allowed(browser, dragged) ||
        !Allowed(browser, target)) {
      return false;
    }
    auto* source_tab = Resolve(dragged);
    auto* target_tab = Resolve(target);
    if (source_tab->IsPinned() != target_tab->IsPinned()) {
      return false;
    }
    const int window_id = browser->GetSessionID().id();
    if (auto split = target_tab->GetSplit();
        split && split != source_tab->GetSplit()) {
      auto* model = target_tab->GetBrowserWindowInterface()->GetTabStripModel();
      if (!model->ListSplits().contains(*split) ||
          model->GetSplitData(*split)->ListTabs().size() >= 4) {
        return false;
      }
    }
    if (!Select(browser, target) || !MoveBlock(dragged, browser, false)) {
      return false;
    }
    source_tab = Resolve(dragged);
    target_tab = Resolve(target);
    auto* current = BrowserFor(window_id);
    if (!source_tab || !target_tab || !current ||
        source_tab->GetBrowserWindowInterface() != current ||
        target_tab->GetBrowserWindowInterface() != current) {
      return false;
    }
    auto* model = current->GetTabStripModel();
    const int source_index = model->GetIndexOfTab(source_tab);
    const int target_index = model->GetIndexOfTab(target_tab);
    bool joined = false;
    {
      base::AutoReset<bool> transaction(&mutation_, true);
      joined = model->JoinNoventraSplit(source_index, target_index);
      if (joined && target_tab->GetSplit()) {
        model->UpdateSplitLayout(*target_tab->GetSplit(), layout);
      }
    }
    if (joined) {
      State(current)->selected = dragged;
    }
    Reconcile();
    return joined;
  }

  void Capture(BrowserWindowInterface* browser, const LogicalId& id) {
    auto* state = State(browser);
    auto* tab = Resolve(id);
    if (!state || !tab || tab->GetBrowserWindowInterface() != browser ||
        SpaceId(tab->GetContents()) != state->space ||
        capture_requests_.contains(browser->GetSessionID().id()) ||
        capture_requests_.size() >= kMaximumSnapshots) {
      return;
    }
    auto* contents = tab->GetContents();
    auto* view = contents->GetRenderWidgetHostView();
    state->snapshot.reset();
    const uint64_t request = ++state->snapshot_epoch;
    if (!view || !view->IsShowing()) {
      ScheduleChanged();
      return;
    }
    gfx::Size size = view->GetVisibleViewportSize();
    if (size.IsEmpty()) {
      return;
    }
    const double scale = std::min(
        {1.0, static_cast<double>(kMaximumSnapshotWidth) / size.width(),
         static_cast<double>(kMaximumSnapshotHeight) / size.height()});
    size.SetSize(std::max(1, static_cast<int>(size.width() * scale)),
                 std::max(1, static_cast<int>(size.height() * scale)));
    const uint64_t navigation = EnsureMetadata(contents)->navigation_epoch;
    capture_requests_.emplace(browser->GetSessionID().id(), request);
    view->CopyFromSurface(gfx::Rect(), size, base::Milliseconds(250),
                          base::BindPostTaskToCurrentDefault(base::BindOnce(
                              &Impl::OnCaptured, weak_factory_.GetWeakPtr(),
                              browser->GetSessionID().id(), id, state->space,
                              request, contents->GetWeakPtr(), navigation)));
  }

  void OnCaptured(int window_id,
                  LogicalId id,
                  std::string space,
                  uint64_t request,
                  base::WeakPtr<content::WebContents> contents,
                  uint64_t navigation,
                  const content::CopyFromSurfaceResult& result) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    capture_requests_.erase(window_id);
    auto* browser = BrowserFor(window_id);
    auto* state = State(browser);
    const auto* metadata = contents ? Metadata(contents.get()) : nullptr;
    if (!Enabled() || !state || state->snapshot_epoch != request ||
        state->selected != id || state->space != space || !metadata ||
        metadata->id != id || metadata->navigation_epoch != navigation ||
        SpaceId(contents.get()) != space || !entries_.contains(id) ||
        !result.has_value() || result->bitmap.drawsNothing() ||
        result->bitmap.width() > kMaximumSnapshotWidth ||
        result->bitmap.height() > kMaximumSnapshotHeight ||
        result->bitmap.computeByteSize() >
            static_cast<size_t>(kMaximumSnapshotWidth) *
                kMaximumSnapshotHeight * 4) {
      return;
    }
    size_t images = 0;
    WindowState* oldest = nullptr;
    for (auto& [other_id, window] : windows_) {
      if (window.snapshot) {
        ++images;
        if (other_id != window_id &&
            (!oldest || window.activation < oldest->activation)) {
          oldest = &window;
        }
      }
    }
    if (images >= kMaximumSnapshots && oldest) {
      oldest->snapshot.reset();
    }
    state->snapshot =
        Snapshot{id, space, gfx::ImageSkia::CreateFrom1xBitmap(result->bitmap)};
    ScheduleChanged();
  }

  void OnActivated(BrowserWindowInterface* browser, bool active) {
    if (!ObserveWindow(browser)) {
      return;
    }
    auto* state = State(browser);
    if (state->closing || SessionRestore::IsRestoring(profile_)) {
      return;
    }
    if (active) {
      state->activation = ++activation_;
      if (!state->selected.empty()) {
        Select(browser, state->selected);
      }
    } else if (!state->selected.empty()) {
      Capture(browser, state->selected);
    }
  }

  bool KeepEmptyWindow(BrowserWindowInterface* browser) const {
    const auto* state = State(browser);
    return Enrolled(browser) && state && !quitting_ &&
           !browser_shutdown::IsTryingToQuit() &&
           (mutation_ || (!state->closing && !entries_.empty()));
  }

  CloseResult PrepareWindowClose(BrowserWindowInterface* browser,
                                 bool quitting) {
    if (!Enrolled(browser)) {
      return CloseResult::kUseNormalClose;
    }
    if (mutation_) {
      return CloseResult::kBlocked;
    }
    auto* state = State(browser);
    state->closing = true;
    if (quitting || browser_shutdown::IsTryingToQuit()) {
      if (!quitting_) {
        SaveViewDescriptors(/*for_quit=*/true);
      }
      quitting_ = true;
      return CloseResult::kUseNormalClose;
    }
    SaveViewDescriptors();
    BrowserWindowInterface* survivor = nullptr;
    uint64_t most_recent = 0;
    for (const auto& [id, candidate] : windows_) {
      if (candidate.browser != browser && !candidate.closing &&
          Eligible(profile_, candidate.browser) &&
          (!survivor || candidate.activation >= most_recent)) {
        survivor = candidate.browser;
        most_recent = candidate.activation;
      }
    }
    if (!survivor) {
      return CloseResult::kUseNormalClose;
    }
    const int source_id = browser->GetSessionID().id();
    const int survivor_id = survivor->GetSessionID().id();
    const LogicalId survivor_selection = State(survivor)->selected;
    auto* model = browser->GetTabStripModel();
    while (!model->empty()) {
      auto* tab = model->GetTabAtIndex(0);
      const LogicalId id = EnsureMetadata(tab->GetContents())->id;
      if (!MoveBlock(id, survivor, false)) {
        if (auto* source = BrowserFor(source_id)) {
          State(source)->closing = false;
        }
        return CloseResult::kBlocked;
      }
      auto* source = BrowserFor(source_id);
      survivor = BrowserFor(survivor_id);
      if (!source || !survivor) {
        return CloseResult::kBlocked;
      }
      model = source->GetTabStripModel();
    }
    if (!survivor_selection.empty()) {
      Select(survivor, survivor_selection);
    }
    return CloseResult::kRehomed;
  }

  void WindowCloseCancelled(BrowserWindowInterface* browser) {
    if (auto* state = State(browser)) {
      state->closing = false;
    }
    quitting_ = false;
    ScheduleChanged();
  }

  std::string SerializeWindow(const WindowState& window) const {
    return base::WriteJson(base::DictValue()
                               .Set("version", 1)
                               .Set("window", window.view_id)
                               .Set("selected", window.selected)
                               .Set("space", window.space))
        .value_or(std::string());
  }

  void PersistWindowMetadata() {
    auto* sessions = SessionServiceFactory::GetForProfileIfExisting(profile_);
    if (!sessions) {
      return;
    }
    for (auto& [id, window] : windows_) {
      if (!Eligible(profile_, window.browser)) {
        continue;
      }
      const std::string serialized = SerializeWindow(window);
      if (!serialized.empty() && window.persisted_window != serialized) {
        sessions->AddWindowExtraData(window.browser->GetSessionID(),
                                     kMirrorWindowExtraData, serialized);
        window.persisted_window = serialized;
      }
    }
  }

  void SaveViewDescriptors(bool for_quit = false) {
    if (!Enabled() || restoring_views_ || (quitting_ && !for_quit) ||
        (!for_quit && SessionRestore::IsRestoring(profile_)) ||
        !profile_->GetPrefs()->FindPreference(kWindowMirrorState)) {
      return;
    }
    PersistWindowMetadata();
    base::DictValue views;
    for (const auto& [id, window] : windows_) {
      if (views.size() >= kMaximumRestoredViews) {
        break;
      }
      // Native sessions persist populated windows and own their documents.
      // Only empty projections need a companion descriptor.
      if ((!for_quit && window.closing) || !window.model->empty() ||
          !ValidId(window.view_id) || !ValidId(window.selected) ||
          !SpaceExists(window.space) || !Eligible(profile_, window.browser)) {
        continue;
      }
      const auto selected = entries_.find(window.selected);
      if (selected == entries_.end() ||
          selected->second.row.space != window.space ||
          !selected->second.row.handle.Get() || !window.browser->GetWindow()) {
        continue;
      }
      const gfx::Rect bounds = window.browser->GetWindow()->GetRestoredBounds();
      views.Set(window.view_id,
                base::DictValue()
                    .Set("version", 1)
                    .Set("selected", window.selected)
                    .Set("space", window.space)
                    .Set("x", std::clamp(bounds.x(), -1000000, 1000000))
                    .Set("y", std::clamp(bounds.y(), -1000000, 1000000))
                    .Set("width", std::clamp(bounds.width(), 100, 16384))
                    .Set("height", std::clamp(bounds.height(), 100, 16384)));
    }
    if (views != profile_->GetPrefs()->GetDict(kWindowMirrorState)) {
      profile_->GetPrefs()->SetDict(kWindowMirrorState, std::move(views));
    }
  }

  void OnSessionRestored(Profile* profile, int tabs_restored) {
    if (profile != profile_ || tabs_restored <= 0 || restored_views_ ||
        !Enabled()) {
      return;
    }
    // Wait until all other completion callbacks have applied window metadata.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&Impl::RestoreMissingViews, weak_factory_.GetWeakPtr()));
  }

  void RestoreMissingViews() {
    if (!Enabled() || restored_views_ || mutation_ ||
        SessionRestore::IsRestoring(profile_)) {
      return;
    }
    Reconcile();
    // Calling this on ordinary startup cannot create windows without actual
    // restored logical IDs. No descriptor ever supplies a page URL.
    if (entries_.empty()) {
      return;
    }
    restored_views_ = true;
    {
      base::AutoReset<bool> guard(&restoring_views_, true);
      size_t restored = 0;
      for (const auto [view_id, value] : startup_views_) {
        if (restored >= kMaximumRestoredViews) {
          break;
        }
        const auto* descriptor = value.GetIfDict();
        if (!ValidId(view_id) || !descriptor ||
            descriptor->FindInt("version") != 1) {
          continue;
        }
        const auto* selected = descriptor->FindString("selected");
        const auto* space = descriptor->FindString("space");
        const auto x = descriptor->FindInt("x");
        const auto y = descriptor->FindInt("y");
        const auto width = descriptor->FindInt("width");
        const auto height = descriptor->FindInt("height");
        if (!selected || !ValidId(*selected) || !space ||
            !SpaceExists(*space) || !x || !y || !width || !height ||
            *x < -1000000 || *x > 1000000 || *y < -1000000 || *y > 1000000 ||
            *width < 100 || *width > 16384 || *height < 100 ||
            *height > 16384) {
          continue;
        }
        const auto actual = entries_.find(*selected);
        if (actual == entries_.end() || actual->second.row.space != *space ||
            !actual->second.row.handle.Get() ||
            std::ranges::any_of(windows_, [&](const auto& pair) {
              return pair.second.view_id == view_id;
            })) {
          continue;
        }
        BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                         profile_, false);
        params.should_trigger_session_restore = false;
        params.creation_source =
            BrowserWindowCreateParams::CreationSource::kSessionRestore;
        params.initial_bounds = gfx::Rect(*x, *y, *width, *height);
        auto* browser = CreateBrowserWindow(std::move(params));
        if (!browser || !ObserveWindow(browser)) {
          continue;
        }
        auto* window = State(browser);
        window->view_id = view_id;
        window->selected = *selected;
        window->space = *space;
        window->snapshot.reset();
        ++window->snapshot_epoch;
        if (browser->GetWindow()) {
          browser->GetWindow()->ShowInactive();
        }
        ++restored;
      }
    }
    startup_views_.clear();
    Reconcile();
    // Activation during native restore intentionally did not move documents.
    // Claim only the one actual active window after native owners are complete.
    for (auto& [id, window] : windows_) {
      if (!window.closing && window.browser->GetWindow() &&
          window.browser->GetWindow()->IsActive()) {
        OnActivated(window.browser, true);
        break;
      }
    }
    SaveViewDescriptors();
  }

  void OnPreferenceChanged() {
    if (shutdown_) {
      return;
    }
    if (Enabled()) {
      ProfileBrowserCollection::GetForProfile(profile_)->ForEach(
          [this](BrowserWindowInterface* browser) {
            ObserveWindow(browser);
            return true;
          });
      Reconcile();
    } else {
      for (auto& [id, entry] : entries_) {
        will_detach_.Notify(entry.row.handle);
      }
      entries_.clear();
      startup_views_.clear();
      if (profile_->GetPrefs()->FindPreference(kWindowMirrorState)) {
        profile_->GetPrefs()->SetDict(kWindowMirrorState, base::DictValue());
      }
      ScheduleEmptyShellCleanup();
      for (auto& [id, state] : windows_) {
        state.snapshot.reset();
        ++state.snapshot_epoch;
      }
      ScheduleChanged();
    }
  }

  void OnSpacePreferencesChanged() {
    for (auto& [id, state] : windows_) {
      state.snapshot.reset();
      ++state.snapshot_epoch;
    }
    ScheduleReconcile();
  }

  void OnBrowserCreated(BrowserWindowInterface* browser) override {
    ObserveWindow(browser);
  }

  void OnBrowserClosed(BrowserWindowInterface* browser) override {
    const auto it = windows_.find(browser->GetSessionID().id());
    if (it == windows_.end() || it->second.browser != browser) {
      return;
    }
    it->second.model->RemoveObserver(this);
    capture_requests_.erase(it->first);
    windows_.erase(it);
    ScheduleReconcile();
  }

  void OnTabWillBeRemoved(tabs::TabInterface* tab, int index) override {
    // No listener may mutate models from this notification. It exists solely
    // to release borrowed view references before native deletion/detachment.
    will_detach_.Notify(tab->GetHandle());
  }

  void OnTabStripModelChanged(
      TabStripModel* model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    if (!Enabled() || mutation_) {
      return;
    }
    for (auto& [id, window] : windows_) {
      if (window.model == model && selection.active_tab_changed() &&
          selection.new_tab) {
        auto* contents = selection.new_tab->GetContents();
        window.space = SpaceId(contents);
        window.selected = EnsureMetadata(contents)->id;
        window.snapshot.reset();
        ++window.snapshot_epoch;
        break;
      }
    }
    ScheduleReconcile();
  }

  void OnTabChangedAt(tabs::TabInterface* tab,
                      TabChangeType change_type) override {
    ScheduleReconcile();
  }
  void OnTabPinnedStateChanged(tabs::TabInterface* tab, int index) override {
    ScheduleReconcile();
  }
  void OnTabGroupChanged(const TabGroupChange& change) override {
    ScheduleReconcile();
  }
  void OnSplitTabChanged(const SplitTabChange& change) override {
    ScheduleReconcile();
  }
  void OnTabStripModelDestroyed(TabStripModel* model) override {
    for (auto it = windows_.begin(); it != windows_.end();) {
      if (it->second.model == model) {
        capture_requests_.erase(it->first);
        it = windows_.erase(it);
      } else {
        ++it;
      }
    }
    ScheduleReconcile();
  }

  void Shutdown() {
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
    weak_factory_.InvalidateWeakPtrs();
    prefs_.RemoveAll();
    collection_.Reset();
    restored_subscription_ = {};
    for (auto& [id, window] : windows_) {
      window.model->RemoveObserver(this);
    }
    windows_.clear();
    entries_.clear();
  }

  const raw_ptr<Profile> profile_;
  PrefChangeRegistrar prefs_;
  base::CallbackListSubscription restored_subscription_;
  base::DictValue startup_views_;
  base::ScopedObservation<ProfileBrowserCollection, BrowserCollectionObserver>
      collection_{this};
  std::map<int, WindowState> windows_;
  std::map<LogicalId, Entry> entries_;
  std::vector<Recovery> recovery_;
  std::map<int, uint64_t> capture_requests_;
  base::RepeatingClosureList changed_;
  base::RepeatingCallbackList<void(tabs::TabHandle)> will_detach_;
  uint64_t activation_ = 0;
  bool shutdown_ = false;
  bool mutation_ = false;
  bool quitting_ = false;
  bool reconciling_ = false;
  bool notify_pending_ = false;
  bool reconcile_pending_ = false;
  bool empty_cleanup_pending_ = false;
  bool restoring_views_ = false;
  bool restored_views_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

class WindowMirrorServiceFactory final : public ProfileKeyedServiceFactory {
 public:
  static WindowMirrorServiceFactory* GetInstance() {
    static base::NoDestructor<WindowMirrorServiceFactory> instance;
    return instance.get();
  }
  static WindowMirrorService* Get(Profile* profile, bool create = true) {
    return static_cast<WindowMirrorService*>(
        GetInstance()->GetServiceForBrowserContext(profile, create));
  }

 private:
  friend class base::NoDestructor<WindowMirrorServiceFactory>;
  WindowMirrorServiceFactory()
      : ProfileKeyedServiceFactory(
            "NoventraWindowMirror",
            ProfileSelections::Builder()
                .WithRegular(ProfileSelection::kOriginalOnly)
                .WithGuest(ProfileSelection::kNone)
                .WithSystem(ProfileSelection::kNone)
                .WithAshInternals(ProfileSelection::kNone)
                .Build()) {
    DependsOn(BrowserManagerServiceFactory::GetInstance());
    DependsOn(SessionServiceFactory::GetInstance());
  }
  ~WindowMirrorServiceFactory() override = default;
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    auto* profile = Profile::FromBrowserContext(context);
    if (!profile->IsRegularProfile() || profile->IsOffTheRecord()) {
      return nullptr;
    }
    return std::unique_ptr<KeyedService>(new WindowMirrorService(profile));
  }
};

// static
WindowMirrorService* WindowMirrorService::GetForProfile(Profile* profile) {
  if (!profile || !profile->IsRegularProfile() || profile->IsOffTheRecord() ||
      !profile->GetPrefs()->FindPreference(kWindowMirroring)) {
    return nullptr;
  }
  return WindowMirrorServiceFactory::Get(profile);
}
// static
WindowMirrorService* WindowMirrorService::GetForProfileIfExisting(
    Profile* profile) {
  if (!profile || !profile->IsRegularProfile() || profile->IsOffTheRecord() ||
      !profile->GetPrefs()->FindPreference(kWindowMirroring)) {
    return nullptr;
  }
  return WindowMirrorServiceFactory::Get(profile, false);
}
// static
bool WindowMirrorService::IsEnabled(Profile* profile) {
  return profile && profile->IsRegularProfile() && !profile->IsOffTheRecord() &&
         profile->GetPrefs()->FindPreference(kWindowMirroring) &&
         profile->GetPrefs()->GetBoolean(kWindowMirroring);
}
// static
void WindowMirrorService::EnsureFactoryBuilt() {
  WindowMirrorServiceFactory::GetInstance();
}

WindowMirrorService::WindowMirrorService(Profile* profile)
    : impl_(std::make_unique<Impl>(profile)) {}
WindowMirrorService::~WindowMirrorService() = default;
bool WindowMirrorService::IsEnabled() const {
  return impl_->Enabled();
}
bool WindowMirrorService::ObserveWindow(BrowserWindowInterface* browser) {
  return impl_->ObserveWindow(browser);
}
bool WindowMirrorService::IsEnrolled(BrowserWindowInterface* browser) const {
  return impl_->Enrolled(browser);
}
std::vector<WindowMirrorService::Row> WindowMirrorService::Rows(
    BrowserWindowInterface* browser) const {
  return impl_->Rows(browser);
}
WindowMirrorService::LogicalId WindowMirrorService::Selected(
    BrowserWindowInterface* browser) const {
  const auto* state = impl_->State(browser);
  return state ? state->selected : LogicalId();
}
std::string WindowMirrorService::SelectedSpace(
    BrowserWindowInterface* browser) const {
  const auto* state = impl_->State(browser);
  return state ? state->space : std::string();
}
bool WindowMirrorService::IsSelectedRemote(
    BrowserWindowInterface* browser) const {
  const auto* state = impl_->State(browser);
  const auto it =
      state ? impl_->entries_.find(state->selected) : impl_->entries_.end();
  return impl_->Enrolled(browser) && it != impl_->entries_.end() &&
         it->second.row.owner_window_id != browser->GetSessionID().id();
}
std::optional<WindowMirrorService::Snapshot> WindowMirrorService::GetSnapshot(
    BrowserWindowInterface* browser) const {
  const auto* state = impl_->State(browser);
  if (!impl_->Enrolled(browser) || !state || !state->snapshot ||
      state->snapshot->id != state->selected ||
      state->snapshot->space != state->space) {
    return std::nullopt;
  }
  return state->snapshot;
}
base::CallbackListSubscription WindowMirrorService::RegisterChanged(
    base::RepeatingClosure cb) {
  return impl_->changed_.Add(std::move(cb));
}
base::CallbackListSubscription WindowMirrorService::RegisterWillDetach(
    base::RepeatingCallback<void(tabs::TabHandle)> cb) {
  return impl_->will_detach_.Add(std::move(cb));
}
bool WindowMirrorService::Select(BrowserWindowInterface* browser,
                                 const LogicalId& id) {
  return impl_->Select(browser, id);
}
bool WindowMirrorService::SelectSpace(BrowserWindowInterface* browser,
                                      const std::string& space) {
  return impl_->SelectSpace(browser, space);
}
bool WindowMirrorService::Close(BrowserWindowInterface* browser,
                                const LogicalId& id) {
  return impl_->Close(browser, id);
}
bool WindowMirrorService::SetPinned(BrowserWindowInterface* browser,
                                    const LogicalId& id,
                                    bool pinned) {
  return impl_->SetPinned(browser, id, pinned);
}
bool WindowMirrorService::MoveLogical(BrowserWindowInterface* browser,
                                      const LogicalId& id,
                                      const LogicalId& before_id) {
  return impl_->MoveLogical(browser, id, before_id);
}
bool WindowMirrorService::DropToSplit(BrowserWindowInterface* browser,
                                      const LogicalId& dragged,
                                      const LogicalId& target,
                                      split_tabs::SplitTabLayout layout) {
  return impl_->DropToSplit(browser, dragged, target, layout);
}
void WindowMirrorService::OnActivated(BrowserWindowInterface* browser,
                                      bool active) {
  impl_->OnActivated(browser, active);
}
bool WindowMirrorService::KeepEmptyWindow(
    BrowserWindowInterface* browser) const {
  return impl_->KeepEmptyWindow(browser);
}
WindowMirrorService::CloseResult WindowMirrorService::PrepareWindowClose(
    BrowserWindowInterface* browser,
    bool quitting) {
  return impl_->PrepareWindowClose(browser, quitting);
}
void WindowMirrorService::WindowCloseCancelled(
    BrowserWindowInterface* browser) {
  impl_->WindowCloseCancelled(browser);
}
void WindowMirrorService::RestoreMissingViews() {
  impl_->RestoreMissingViews();
}
void WindowMirrorService::Shutdown() {
  impl_->Shutdown();
}

// static
void WindowMirrorService::PopulateTabExtraData(
    const content::WebContents* contents,
    std::map<std::string, std::string>* extra_data) {
  if (const auto* state = Metadata(contents); state && ValidId(state->id)) {
    extra_data->insert_or_assign(kMirrorTabExtraData, Serialize(*state));
  }
}
// static
void WindowMirrorService::RestoreTabExtraData(
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  const auto it = extra_data.find(kMirrorTabExtraData);
  if (!contents || it == extra_data.end() ||
      it->second.size() > kMaximumMetadataBytes) {
    return;
  }
  auto value = base::JSONReader::ReadDict(it->second, base::JSON_PARSE_RFC, 4);
  if (!value || value->FindInt("version") != 1) {
    return;
  }
  const auto* id = value->FindString("id");
  const auto order = value->FindInt("order");
  if (!id || !ValidId(*id) || !order || *order < 0 || *order > 1000000) {
    return;
  }
  auto* state = EnsureMetadata(contents);
  state->id = *id;
  state->order = *order;
}
void WindowMirrorService::PopulateWindowExtraData(
    BrowserWindowInterface* browser,
    std::map<std::string, std::string>* extra_data) const {
  const auto* state = impl_->State(browser);
  if (!impl_->Enrolled(browser) || !state) {
    return;
  }
  const std::string json = impl_->SerializeWindow(*state);
  if (!json.empty()) {
    extra_data->insert_or_assign(kMirrorWindowExtraData, json);
  }
}
void WindowMirrorService::RestoreWindowExtraData(
    BrowserWindowInterface* browser,
    const std::map<std::string, std::string>& extra_data) {
  if (!impl_->ObserveWindow(browser)) {
    return;
  }
  const auto it = extra_data.find(kMirrorWindowExtraData);
  if (it == extra_data.end() || it->second.size() > kMaximumMetadataBytes) {
    return;
  }
  auto value = base::JSONReader::ReadDict(it->second, base::JSON_PARSE_RFC, 4);
  if (!value || value->FindInt("version") != 1) {
    return;
  }
  const auto* selected = value->FindString("selected");
  const auto* space = value->FindString("space");
  if (!selected || !ValidId(*selected) || !space ||
      !impl_->SpaceExists(*space)) {
    return;
  }
  auto* state = impl_->State(browser);
  if (const auto* view = value->FindString("window"); view && ValidId(*view)) {
    const bool duplicate =
        std::ranges::any_of(impl_->windows_, [&](const auto& window) {
          return window.second.browser != browser &&
                 window.second.view_id == *view;
        });
    if (!duplicate) {
      state->view_id = *view;
    }
  }
  state->selected = *selected;
  state->space = *space;
  state->snapshot.reset();
  ++state->snapshot_epoch;
  impl_->Reconcile();
}

}  // namespace noventra
