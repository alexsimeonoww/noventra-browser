// Copyright 2026 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/ui/accelerator_table.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_metadata.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_prefs.h"
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_serialization.h"
#include "chrome/browser/ui/noventra_commands.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/text_utils.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/ui/browser_shortcuts/browser_shortcut_platform_mac.h"
#endif

namespace browser_shortcuts {

namespace {

constexpr char kAddedKey[] = "added";
constexpr char kRemovedKey[] = "removed";

std::string CommandIdToPrefKey(int command_id) {
  return base::NumberToString(command_id);
}

std::optional<int> PrefKeyToCommandId(std::string_view key) {
  int command_id = 0;
  if (!base::StringToInt(key, &command_id)) {
    return std::nullopt;
  }
  return command_id;
}

bool ContainsAccelerator(
    const std::vector<BrowserShortcutService::AcceleratorEntry>& entries,
    const ui::Accelerator& accelerator) {
  auto it = std::ranges::lower_bound(
      entries, accelerator, std::ranges::less(),
      &BrowserShortcutService::AcceleratorEntry::accelerator);
  return it != entries.end() && it->accelerator == accelerator;
}

bool InsertEntrySorted(
    std::vector<BrowserShortcutService::AcceleratorEntry>& entries,
    BrowserShortcutService::AcceleratorEntry entry) {
  auto it = std::ranges::lower_bound(
      entries, entry.accelerator, std::ranges::less(),
      &BrowserShortcutService::AcceleratorEntry::accelerator);
  if (it != entries.end() && it->accelerator == entry.accelerator) {
    return false;
  }
  entries.insert(it, std::move(entry));
  return true;
}

std::optional<BrowserShortcutService::AcceleratorEntry> EntryFromString(
    const std::string& serialized,
    bool user_modifiable) {
  std::optional<ui::Accelerator> accelerator =
      AcceleratorFromString(serialized);
  if (!accelerator) {
    return std::nullopt;
  }
  if (AcceleratorToString(*accelerator).empty()) {
    return std::nullopt;
  }
  return BrowserShortcutService::AcceleratorEntry{*accelerator,
                                                  user_modifiable};
}

std::string EntryToString(
    const BrowserShortcutService::AcceleratorEntry& entry) {
  return AcceleratorToString(entry.accelerator);
}

std::u16string GetCommandDisplayName(const BrowserShortcutCommandLabel& label) {
  if (label.message_id == 0 && label.replacement) {
    return *label.replacement;
  }
  std::u16string display_name = gfx::RemoveAccelerator(
      label.replacement.has_value()
          ? l10n_util::GetStringFUTF16(label.message_id, *label.replacement)
          : l10n_util::GetStringUTF16(label.message_id));
  base::TrimWhitespace(display_name, base::TRIM_TRAILING, &display_name);
  if (display_name.ends_with(u"...")) {
    display_name.resize(display_name.size() - 3);
  } else if (!display_name.empty() && display_name.back() == 0x2026) {
    display_name.pop_back();
  }
  return display_name;
}

}  // namespace

BrowserShortcutService::BrowserShortcutService(PrefService* prefs)
    : prefs_(prefs) {
  pref_change_registrar_.Init(prefs_);
  const auto on_label_pref_changed = base::BindRepeating(
      &BrowserShortcutService::OnCommandLabelsChanged, base::Unretained(this));
  pref_change_registrar_.Add(::prefs::kCopyPageUrlShortcut,
                             on_label_pref_changed);
  pref_change_registrar_.Add(::prefs::kHeliumLayout, on_label_pref_changed);
  pref_change_registrar_.Add(::prefs::kVerticalCollapseShortcut,
                             on_label_pref_changed);

  LoadFromPrefs();
  RebuildCommandCache();
}

BrowserShortcutService::~BrowserShortcutService() = default;

const std::vector<CommandInfo>& BrowserShortcutService::GetCommands() const {
  return command_cache_;
}

std::vector<std::pair<ui::Accelerator, int>>
BrowserShortcutService::GetAcceleratorMap() const {
  std::vector<std::pair<ui::Accelerator, int>> map;
  for (const auto& [command_id, entries] : current_) {
    for (const AcceleratorEntry& entry : entries) {
      map.emplace_back(entry.accelerator, command_id);
    }
  }
  return map;
}

bool BrowserShortcutService::HasCommand(int command_id) const {
  return defaults_.contains(command_id);
}

bool BrowserShortcutService::HasDefaultAccelerator(
    int command_id,
    const ui::Accelerator& accelerator) const {
  const auto it = defaults_.find(command_id);
  return it != defaults_.end() && ContainsAccelerator(it->second, accelerator);
}

AssignmentResult BrowserShortcutService::AssignAcceleratorToCommand(
    int command_id,
    const std::string& serialized) {
  if (!HasCommand(command_id)) {
    return AssignmentResult::kUnknownCommand;
  }
  std::optional<AcceleratorEntry> entry = EntryFromString(serialized, true);
  if (!entry) {
    return AssignmentResult::kInvalidAccelerator;
  }
  return AssignAcceleratorToCommand(command_id, std::move(*entry));
}

AssignmentResult BrowserShortcutService::AssignAcceleratorToCommand(
    int command_id,
    AcceleratorEntry entry) {
  if (!HasCommand(command_id)) {
    return AssignmentResult::kUnknownCommand;
  }
  if (IsAcceleratorLocked(entry.accelerator, command_id)) {
    return AssignmentResult::kShortcutLocked;
  }

  const bool removed_conflict =
      RemoveConflictingAccelerators(entry.accelerator, command_id);
  const bool inserted =
      InsertEntrySorted(current_[command_id], std::move(entry));
  if (!removed_conflict && !inserted) {
    return AssignmentResult::kSuccess;
  }

  PersistToPrefs();
  RebuildCommandCache();
  NotifyChanged();
  return AssignmentResult::kSuccess;
}

bool BrowserShortcutService::UnassignAcceleratorFromCommand(
    int command_id,
    const std::string& serialized) {
  std::optional<AcceleratorEntry> removed_entry =
      EntryFromString(serialized, true);
  if (!removed_entry) {
    return false;
  }
  return UnassignAcceleratorFromCommand(command_id, removed_entry->accelerator);
}

bool BrowserShortcutService::UnassignAcceleratorFromCommand(
    int command_id,
    const ui::Accelerator& accelerator) {
  std::vector<AcceleratorEntry>* entries = FindMutableEntries(command_id);
  if (!entries) {
    return false;
  }
  const size_t old_size = entries->size();
  std::erase_if(*entries, [&](const AcceleratorEntry& entry) {
    return entry.user_modifiable && entry.accelerator == accelerator;
  });
  if (entries->size() == old_size) {
    return false;
  }
  PersistToPrefs();
  RebuildCommandCache();
  NotifyChanged();
  return true;
}

bool BrowserShortcutService::ResetAcceleratorsForCommand(int command_id) {
  const auto defaults_it = defaults_.find(command_id);
  if (defaults_it == defaults_.end()) {
    return false;
  }
  current_[command_id] = defaults_it->second;
  for (const AcceleratorEntry& entry : defaults_it->second) {
    RemoveConflictingAccelerators(entry.accelerator, command_id);
  }
  PersistToPrefs();
  RebuildCommandCache();
  NotifyChanged();
  return true;
}

void BrowserShortcutService::ResetAccelerators() {
  current_ = defaults_;
  PersistToPrefs(/*preserve_inactive_deltas=*/false);
  RebuildCommandCache();
  NotifyChanged();
}

std::optional<int> BrowserShortcutService::FindCommandForAccelerator(
    const std::string& serialized) const {
  std::optional<AcceleratorEntry> search_entry =
      EntryFromString(serialized, true);
  if (!search_entry) {
    return std::nullopt;
  }
  return FindCommandForAccelerator(search_entry->accelerator);
}

std::optional<int> BrowserShortcutService::FindCommandForAccelerator(
    const ui::Accelerator& accelerator) const {
  for (const auto& [command_id, entries] : current_) {
    for (const AcceleratorEntry& entry : entries) {
      if (entry.accelerator == accelerator) {
        return command_id;
      }
    }
  }
  return std::nullopt;
}

bool BrowserShortcutService::IsAcceleratorLocked(
    const std::string& serialized,
    int command_id_to_ignore) const {
  std::optional<AcceleratorEntry> search_entry =
      EntryFromString(serialized, true);
  if (!search_entry) {
    return false;
  }
  return IsAcceleratorLocked(search_entry->accelerator, command_id_to_ignore);
}

bool BrowserShortcutService::IsAcceleratorLocked(
    const ui::Accelerator& accelerator,
    int command_id_to_ignore) const {
#if BUILDFLAG(IS_MAC)
  for (const auto& platform_default : GetPlatformDefaultAccelerators()) {
    if (platform_default.command_id != command_id_to_ignore &&
        !platform_default.user_modifiable &&
        platform_default.accelerator == accelerator) {
      return true;
    }
  }
#endif

  for (const auto& [command_id, entries] : current_) {
    if (command_id == command_id_to_ignore) {
      continue;
    }
    for (const AcceleratorEntry& entry : entries) {
      if (!entry.user_modifiable && entry.accelerator == accelerator) {
        return true;
      }
    }
  }
  return false;
}

void BrowserShortcutService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void BrowserShortcutService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void BrowserShortcutService::LoadFromPrefs() {
  defaults_ = BuildCurrentDefaults();
  current_ = defaults_;

  bool pref_needs_cleanup = false;
  for (const auto& [command_id, delta] :
       ReadAcceleratorDeltasPref(&pref_needs_cleanup)) {
    auto current_it = current_.find(command_id);
    if (current_it == current_.end()) {
      continue;
    }

    for (const AcceleratorEntry& entry : delta.removed) {
      auto removed = entry.accelerator;
      // A removed old Space default represents an explicit unbind or remap.
      // Carry it to the horizontal default; user-added bindings stay intact.
      const bool old_next = command_id == IDC_NOVENTRA_NEXT_SPACE &&
                            removed.key_code() == ui::VKEY_DOWN;
      const bool old_previous = command_id == IDC_NOVENTRA_PREVIOUS_SPACE &&
                                removed.key_code() == ui::VKEY_UP;
      if ((old_next || old_previous) &&
          removed.modifiers() == (ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN)) {
        removed = ui::Accelerator(old_next ? ui::VKEY_RIGHT : ui::VKEY_LEFT,
                                  removed.modifiers());
        pref_needs_cleanup = true;
      }
      const size_t erased = std::erase_if(
          current_it->second, [&](const AcceleratorEntry& current) {
            return current.user_modifiable &&
                   current.accelerator == removed;
          });
      pref_needs_cleanup = pref_needs_cleanup || erased == 0;
    }

    for (const AcceleratorEntry& entry : delta.added) {
      if (IsAcceleratorLocked(entry.accelerator, command_id)) {
        pref_needs_cleanup = true;
        continue;
      }
      RemoveConflictingAccelerators(entry.accelerator, command_id);
      const bool inserted = InsertEntrySorted(current_it->second, entry);
      pref_needs_cleanup = pref_needs_cleanup || !inserted;
    }
  }

  if (pref_needs_cleanup) {
    PersistToPrefs();
  }
}

void BrowserShortcutService::PersistToPrefs(bool preserve_inactive_deltas) {
  base::DictValue pref;
  const base::DictValue& existing_pref =
      prefs_->GetDict(prefs::kCustomAccelerators);

  if (preserve_inactive_deltas) {
    for (auto [command_key, value] : existing_pref) {
      std::optional<int> command_id = PrefKeyToCommandId(command_key);
      if (command_id && !defaults_.contains(*command_id) && value.is_dict() &&
          GetBrowserShortcutCommandLabel(*command_id, prefs_).has_value()) {
        pref.Set(command_key, value.Clone());
      }
    }
  }

  for (const auto& [command_id, entries] : current_) {
    const auto defaults_it = defaults_.find(command_id);
    if (defaults_it == defaults_.end()) {
      continue;
    }

    base::ListValue added;
    for (const AcceleratorEntry& entry : entries) {
      if (!ContainsAccelerator(defaults_it->second, entry.accelerator)) {
        added.Append(EntryToString(entry));
      }
    }

    base::ListValue removed;
    for (const AcceleratorEntry& entry : defaults_it->second) {
      if (entry.user_modifiable &&
          !ContainsAccelerator(entries, entry.accelerator)) {
        removed.Append(EntryToString(entry));
      }
    }

    if (added.empty() && removed.empty()) {
      continue;
    }

    base::DictValue delta;
    if (!added.empty()) {
      delta.Set(kAddedKey, std::move(added));
    }
    if (!removed.empty()) {
      delta.Set(kRemovedKey, std::move(removed));
    }
    pref.Set(CommandIdToPrefKey(command_id), std::move(delta));
  }

  if (pref.empty()) {
    prefs_->ClearPref(prefs::kCustomAccelerators);
    return;
  }

  prefs_->SetDict(prefs::kCustomAccelerators, std::move(pref));
}

void BrowserShortcutService::RebuildCommandCache() {
  command_cache_.clear();
  for (const auto& [command_id, entries] : current_) {
    std::optional<BrowserShortcutCommandLabel> label =
        GetBrowserShortcutCommandLabel(command_id, prefs_);
    if (!label.has_value()) {
      continue;
    }
    CommandInfo command;
    command.command_id = command_id;
    command.name = GetCommandDisplayName(*label);
    command.modified = !HasDefaultEquivalent(command_id);
    for (const AcceleratorEntry& entry : entries) {
      std::string serialized = EntryToString(entry);
      if (serialized.empty()) {
        continue;
      }
      command.accelerators.push_back(
          {serialized, AcceleratorToDisplayString(entry.accelerator),
           entry.user_modifiable});
    }
    command_cache_.push_back(std::move(command));
  }
}

void BrowserShortcutService::NotifyChanged() {
  for (Observer& observer : observers_) {
    observer.OnBrowserShortcutsChanged();
  }
}

void BrowserShortcutService::OnCommandLabelsChanged() {
  LoadFromPrefs();
  RebuildCommandCache();
  NotifyChanged();
}

BrowserShortcutService::CommandAccelerators
BrowserShortcutService::BuildCurrentDefaults() const {
  CommandAccelerators defaults;
  base::flat_set<ui::Accelerator> seen_accelerators;
  auto insert_default = [&](int command_id,
                            const ui::Accelerator& ui_accelerator,
                            bool user_modifiable, bool reserve_unlabeled) {
    std::string serialized = AcceleratorToString(ui_accelerator);
    if (serialized.empty()) {
      return;
    }
    const bool has_label =
        GetBrowserShortcutCommandLabel(command_id, prefs_).has_value();
    if (!has_label && !reserve_unlabeled) {
      return;
    }
    if (!seen_accelerators.insert(ui_accelerator).second) {
      return;
    }
    if (!has_label) {
      // Keep the accelerator reserved even though this Settings page cannot
      // display the owning command.
      return;
    }
    AcceleratorEntry entry = {ui_accelerator, user_modifiable};
    InsertEntrySorted(defaults[command_id], std::move(entry));
  };

#if BUILDFLAG(IS_MAC)
  for (const BrowserShortcutPlatformDefaultAccelerator& accelerator :
       GetPlatformDefaultAccelerators()) {
    // Reserve unlabeled platform shortcuts too, so visible commands cannot
    // claim macOS defaults that are dispatched outside this Settings surface.
    insert_default(accelerator.command_id, accelerator.accelerator,
                   accelerator.user_modifiable,
                   /*reserve_unlabeled=*/true);
  }
#endif

  for (const AcceleratorMapping& accelerator : GetAcceleratorList()) {
    ui::Accelerator ui_accelerator(accelerator.keycode, accelerator.modifiers);
    // Browser accelerator dispatch can only route one command per accelerator.
    // Locked platform defaults are inserted first, so they own conflicts with
    // user-modifiable accelerator-table defaults.
    insert_default(
        accelerator.command_id, ui_accelerator,
        IsBrowserShortcutDefaultUserModifiable(accelerator.command_id),
        /*reserve_unlabeled=*/false);
  }
  for (int command_id : GetBrowserShortcutCommandsWithoutDefault(prefs_)) {
    defaults.try_emplace(command_id);
  }
  return defaults;
}

BrowserShortcutService::CommandAcceleratorDeltas
BrowserShortcutService::ReadAcceleratorDeltasPref(
    bool* pref_needs_cleanup) const {
  CommandAcceleratorDeltas result;
  const base::DictValue& pref = prefs_->GetDict(prefs::kCustomAccelerators);
  for (auto [command_key, value] : pref) {
    std::optional<int> command_id = PrefKeyToCommandId(command_key);
    const base::DictValue* delta = value.GetIfDict();
    if (!command_id || !delta) {
      *pref_needs_cleanup = true;
      continue;
    }
    if (!defaults_.contains(*command_id)) {
      continue;
    }
    AcceleratorDelta parsed_delta;
    parsed_delta.added =
        ReadAcceleratorDeltaList(*delta, kAddedKey, pref_needs_cleanup);
    parsed_delta.removed =
        ReadAcceleratorDeltaList(*delta, kRemovedKey, pref_needs_cleanup);
    if (parsed_delta.added.empty() && parsed_delta.removed.empty()) {
      *pref_needs_cleanup = true;
      continue;
    }
    result.emplace(*command_id, std::move(parsed_delta));
  }
  return result;
}

std::vector<BrowserShortcutService::AcceleratorEntry>
BrowserShortcutService::ReadAcceleratorDeltaList(
    const base::DictValue& pref,
    std::string_view key,
    bool* pref_needs_cleanup) const {
  std::vector<AcceleratorEntry> entries;
  const base::ListValue* accelerators = pref.FindList(key);
  if (!accelerators) {
    return entries;
  }
  for (const base::Value& item : *accelerators) {
    const std::string* item_string = item.GetIfString();
    if (!item_string) {
      *pref_needs_cleanup = true;
      continue;
    }
    std::optional<AcceleratorEntry> entry = EntryFromString(*item_string, true);
    if (!entry) {
      *pref_needs_cleanup = true;
      continue;
    }
    if (!InsertEntrySorted(entries, std::move(*entry))) {
      *pref_needs_cleanup = true;
    }
  }
  return entries;
}

bool BrowserShortcutService::RemoveConflictingAccelerators(
    const ui::Accelerator& accelerator,
    int command_id_to_keep) {
  bool removed = false;
  for (auto& [command_id, entries] : current_) {
    if (command_id == command_id_to_keep) {
      continue;
    }
    const size_t erased =
        std::erase_if(entries, [&](const AcceleratorEntry& entry) {
          return entry.user_modifiable && entry.accelerator == accelerator;
        });
    removed = removed || erased > 0;
  }
  return removed;
}

std::vector<BrowserShortcutService::AcceleratorEntry>*
BrowserShortcutService::FindMutableEntries(int command_id) {
  const auto it = current_.find(command_id);
  return it == current_.end() ? nullptr : &it->second;
}

bool BrowserShortcutService::HasDefaultEquivalent(int command_id) const {
  const auto defaults_it = defaults_.find(command_id);
  const auto current_it = current_.find(command_id);
  if (defaults_it == defaults_.end() || current_it == current_.end()) {
    return false;
  }
  return defaults_it->second == current_it->second;
}

}  // namespace browser_shortcuts
