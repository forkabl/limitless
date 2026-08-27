// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_workspace_controller.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/common/url_constants.h"
#include "components/split_tabs/split_tab_id.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"

namespace limitless {
namespace {

constexpr int kWorkspaceSchemaVersion = 1;
constexpr size_t kMaximumWorkspaces = 32;
constexpr size_t kMaximumWorkspaceNameCodeUnits = 64;
constexpr size_t kMaximumWorkspaceWindowDataBytes = 32 * 1024;
constexpr size_t kMaximumWorkspaceTabDataBytes = 1024;

using ControllerMap = std::map<BrowserWindowInterface*, WorkspaceController*>;

ControllerMap& GetControllers() {
  static base::NoDestructor<ControllerMap> controllers;
  return *controllers;
}

std::string NewIdentifier() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

bool IsCanonicalIdentifier(const std::string& identifier) {
  const base::Uuid parsed = base::Uuid::ParseLowercase(identifier);
  return parsed.is_valid() && parsed.AsLowercaseString() == identifier;
}

bool IsUnsafeNameCodeUnit(char16_t code_unit) {
  if (code_unit < 0x20 || code_unit == 0x7f) {
    return true;
  }
  switch (code_unit) {
    case 0x061c:
    case 0x200e:
    case 0x200f:
    case 0x202a:
    case 0x202b:
    case 0x202c:
    case 0x202d:
    case 0x202e:
    case 0x2066:
    case 0x2067:
    case 0x2068:
    case 0x2069:
      return true;
    default:
      return false;
  }
}

std::optional<std::u16string> ValidateWorkspaceName(
    std::u16string_view candidate) {
  std::u16string name(base::TrimWhitespace(candidate, base::TRIM_ALL));
  if (name.empty() || name.size() > kMaximumWorkspaceNameCodeUnits ||
      std::ranges::any_of(name, IsUnsafeNameCodeUnit)) {
    return std::nullopt;
  }
  return name;
}

class WorkspaceTabUserData final
    : public content::WebContentsUserData<WorkspaceTabUserData> {
 public:
  ~WorkspaceTabUserData() override = default;

  const std::string& tab_identifier() const { return tab_identifier_; }
  const std::string& workspace_identifier() const {
    return workspace_identifier_;
  }

  void Update(std::string tab_identifier, std::string workspace_identifier) {
    tab_identifier_ = std::move(tab_identifier);
    workspace_identifier_ = std::move(workspace_identifier);
  }

 private:
  friend WebContentsUserData;

  WorkspaceTabUserData(content::WebContents* contents,
                       std::string tab_identifier,
                       std::string workspace_identifier)
      : content::WebContentsUserData<WorkspaceTabUserData>(*contents),
        tab_identifier_(std::move(tab_identifier)),
        workspace_identifier_(std::move(workspace_identifier)) {}

  std::string tab_identifier_;
  std::string workspace_identifier_;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(WorkspaceTabUserData);

struct ParsedWorkspaceTab {
  std::string tab_identifier;
  std::string workspace_identifier;
};

std::optional<ParsedWorkspaceTab> ParseWorkspaceTab(
    const std::string& serialized) {
  if (serialized.empty() || serialized.size() > kMaximumWorkspaceTabDataBytes) {
    return std::nullopt;
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& dict = value->GetDict();
  const std::optional<int> version = dict.FindInt("version");
  const std::string* tab_identifier = dict.FindString("tab_id");
  const std::string* workspace_identifier = dict.FindString("workspace_id");
  if (version != kWorkspaceSchemaVersion || !tab_identifier ||
      !workspace_identifier || !IsCanonicalIdentifier(*tab_identifier) ||
      !IsCanonicalIdentifier(*workspace_identifier)) {
    return std::nullopt;
  }
  return ParsedWorkspaceTab{
      .tab_identifier = *tab_identifier,
      .workspace_identifier = *workspace_identifier,
  };
}

std::optional<std::string> SerializeWorkspaceTab(
    const std::string& tab_identifier,
    const std::string& workspace_identifier) {
  base::DictValue root;
  root.Set("version", kWorkspaceSchemaVersion);
  root.Set("tab_id", tab_identifier);
  root.Set("workspace_id", workspace_identifier);
  std::string serialized;
  if (!base::JSONWriter::Write(root, &serialized) ||
      serialized.size() > kMaximumWorkspaceTabDataBytes) {
    return std::nullopt;
  }
  return serialized;
}

}  // namespace

bool AreWorkspacesEnabled(const base::CommandLine& command_line) {
  if (!command_line.HasSwitch(kWorkspacesSwitch)) {
    return true;
  }
  return command_line.GetSwitchValueASCII(kWorkspacesSwitch) == "enabled";
}

std::u16string GetDefaultWorkspaceName(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(kDefaultWorkspaceNameSwitch)) {
    const std::string utf8_name =
        command_line.GetSwitchValueUTF8(kDefaultWorkspaceNameSwitch);
    if (base::IsStringUTF8(utf8_name)) {
      if (std::optional<std::u16string> name =
              ValidateWorkspaceName(base::UTF8ToUTF16(utf8_name))) {
        return *name;
      }
    }
  }
  return u"Workspace";
}

WorkspaceController::WorkspaceController(
    BrowserWindowInterface* browser_window,
    WorkspacePersistenceCallbacks persistence,
    base::RepeatingClosure state_changed)
    : enabled_(AreWorkspacesEnabled(*base::CommandLine::ForCurrentProcess())),
      browser_window_(browser_window),
      tab_list_(TabListInterface::From(browser_window)),
      persistence_(std::move(persistence)),
      state_changed_(std::move(state_changed)) {
  CHECK(browser_window_);
  CHECK(tab_list_);
  CHECK(!GetControllers().contains(browser_window));
  GetControllers().emplace(browser_window, this);
  if (enabled_) {
    CreateFreshState();
    for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
      AssignFreshTab(tab, active_workspace_identifier_);
    }
    UpdateActiveFromChromium();
  }
  tab_list_observation_.Observe(tab_list_);
}

WorkspaceController::~WorkspaceController() {
  Stop();
}

WorkspaceWindowSnapshot WorkspaceController::GetSnapshot() const {
  WorkspaceWindowSnapshot snapshot;
  if (stopped_ || !browser_window_) {
    return snapshot;
  }
  snapshot.enabled = enabled_;
  snapshot.session_restore_in_progress = restoring_;
  if (!enabled_) {
    if (tab_list_) {
      for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
        snapshot.visible_tabs.push_back(tab->GetHandle());
      }
      if (tabs::TabInterface* active = tab_list_->GetActiveTab()) {
        snapshot.active_tab = active->GetHandle();
      }
    }
    return snapshot;
  }
  snapshot.window_identifier = window_identifier_;
  for (size_t index = 0; index < workspaces_.size(); ++index) {
    const WorkspaceRecord& workspace = workspaces_[index];
    snapshot.workspaces.push_back(WorkspaceProjection{
        .identifier = workspace.identifier,
        .name = workspace.name,
        .order = index,
        .is_default = workspace.is_default,
    });
  }
  snapshot.active_workspace_identifier = active_workspace_identifier_;
  snapshot.visible_tabs = GetVisibleTabs();
  for (tabs::TabHandle handle : snapshot.visible_tabs) {
    const TabRecord* record = FindTabRecord(handle);
    if (record) {
      snapshot.visible_tab_projections.push_back({
          .identifier = record->identifier,
          .handle = handle,
      });
    }
  }
  if (tab_list_) {
    tabs::TabInterface* active = tab_list_->GetActiveTab();
    if (active && IsTabInActiveWorkspace(active->GetHandle())) {
      snapshot.active_tab = active->GetHandle();
    }
  }
  return snapshot;
}

WorkspaceOperationResult WorkspaceController::CreateWorkspace(
    const std::u16string& requested_name) {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = window_status};
  }
  std::optional<std::u16string> name = ValidateWorkspaceName(requested_name);
  if (!name) {
    return {.status = WorkspaceOperationStatus::kInvalidName};
  }
  if (workspaces_.size() >= kMaximumWorkspaces) {
    return {.status = WorkspaceOperationStatus::kWorkspaceLimitReached};
  }

  const std::string identifier = NewIdentifier();
  workspaces_.push_back(WorkspaceRecord{
      .identifier = identifier,
      .name = std::move(*name),
  });
  const std::string previous_active = active_workspace_identifier_;
  active_workspace_identifier_ = identifier;
  ++mutation_depth_;
  base::WeakPtr<WorkspaceController> alive = weak_ptr_factory_.GetWeakPtr();
  tabs::TabInterface* opened = OpenRealTabInWorkspace(identifier);
  if (!alive) {
    return {.status = WorkspaceOperationStatus::kWindowUnavailable};
  }
  --mutation_depth_;
  if (!opened) {
    workspaces_.pop_back();
    active_workspace_identifier_ = previous_active;
    PublishNow();
    return {.status = WorkspaceOperationStatus::kOpenTabFailed};
  }
  PersistAll();
  PublishNow();
  return {
      .status = WorkspaceOperationStatus::kSuccess,
      .workspace_identifier = identifier,
  };
}

WorkspaceOperationResult WorkspaceController::SwitchWorkspace(
    const std::string& workspace_identifier) {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = window_status};
  }
  WorkspaceRecord* workspace = FindWorkspace(workspace_identifier);
  if (!workspace) {
    return {.status = WorkspaceOperationStatus::kUnknownWorkspace};
  }

  ++mutation_depth_;
  active_workspace_identifier_ = workspace_identifier;
  std::optional<tabs::TabHandle> target =
      ResolveWorkspaceActivationTarget(*workspace);
  base::WeakPtr<WorkspaceController> alive = weak_ptr_factory_.GetWeakPtr();
  if (target) {
    tab_list_->ActivateTab(*target);
    if (!alive) {
      return {.status = WorkspaceOperationStatus::kWindowUnavailable};
    }
  } else if (!OpenRealTabInWorkspace(workspace_identifier)) {
    if (!alive) {
      return {.status = WorkspaceOperationStatus::kWindowUnavailable};
    }
    --mutation_depth_;
    UpdateActiveFromChromium();
    PublishNow();
    return {.status = WorkspaceOperationStatus::kOpenTabFailed};
  }
  --mutation_depth_;
  UpdateActiveFromChromium();
  PersistAll();
  PublishNow();
  return {.status = WorkspaceOperationStatus::kSuccess};
}

WorkspaceOperationResult WorkspaceController::MoveTabToWorkspace(
    tabs::TabHandle tab,
    const std::string& workspace_identifier) {
  const WorkspaceOperationStatus tab_status = ValidateTab(tab);
  if (tab_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = tab_status};
  }
  WorkspaceRecord* destination = FindWorkspace(workspace_identifier);
  if (!destination) {
    return {.status = WorkspaceOperationStatus::kUnknownWorkspace};
  }
  TabRecord* record = FindTabRecord(tab);
  if (!record) {
    return {.status = WorkspaceOperationStatus::kStaleTab};
  }
  if (record->workspace_identifier == workspace_identifier) {
    return {.status = WorkspaceOperationStatus::kSuccess};
  }

  tabs::TabInterface* tab_interface = tab.Get();
  CHECK(tab_interface);
  if (tab_interface->IsSplit()) {
    const std::optional<split_tabs::SplitTabId> split =
        tab_interface->GetSplit();
    if (!split) {
      return {.status = WorkspaceOperationStatus::kUnsplitFailed};
    }
    tab_list_->Unsplit(*split);
    tab_interface = tab.Get();
    if (!tab_interface || tab_interface->IsSplit() ||
        ValidateTab(tab) != WorkspaceOperationStatus::kSuccess) {
      return {.status = WorkspaceOperationStatus::kUnsplitFailed};
    }
    record = FindTabRecord(tab);
    if (!record) {
      return {.status = WorkspaceOperationStatus::kUnsplitFailed};
    }
  }

  if (WorkspaceRecord* source = FindWorkspace(record->workspace_identifier);
      source && source->last_active_tab_identifier == record->identifier) {
    source->last_active_tab_identifier.reset();
  }
  record->workspace_identifier = workspace_identifier;
  UpdateTabUserData(tab);
  if (tab_list_->GetActiveTab() == tab.Get()) {
    active_workspace_identifier_ = workspace_identifier;
    destination->last_active_tab_identifier = record->identifier;
  }
  PersistAll();
  PublishNow();
  return {.status = WorkspaceOperationStatus::kSuccess};
}

WorkspaceOperationResult WorkspaceController::ActivateTab(tabs::TabHandle tab) {
  const WorkspaceOperationStatus status = ValidateTab(tab);
  if (status != WorkspaceOperationStatus::kSuccess) {
    return {.status = status};
  }
  const TabRecord* record = FindTabRecord(tab);
  if (!record || record->workspace_identifier != active_workspace_identifier_) {
    return {.status = WorkspaceOperationStatus::kForeignWindow};
  }
  tab_list_->ActivateTab(tab);
  return {.status = WorkspaceOperationStatus::kSuccess};
}

WorkspaceOperationResult WorkspaceController::CloseTab(tabs::TabHandle tab) {
  const WorkspaceOperationStatus status = ValidateTab(tab);
  if (status != WorkspaceOperationStatus::kSuccess) {
    return {.status = status};
  }
  const TabRecord* record = FindTabRecord(tab);
  if (!record || record->workspace_identifier != active_workspace_identifier_) {
    return {.status = WorkspaceOperationStatus::kForeignWindow};
  }
  tab_list_->CloseTab(tab);
  return {.status = WorkspaceOperationStatus::kSuccess};
}

WorkspaceOperationResult WorkspaceController::MoveTabToWorkspaceByIdentifier(
    const std::string& window_identifier,
    const std::string& tab_identifier,
    const std::string& workspace_identifier) {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = window_status};
  }
  if (window_identifier != window_identifier_) {
    return {.status = WorkspaceOperationStatus::kForeignWindow};
  }
  std::optional<tabs::TabHandle> tab = FindTabByIdentifier(tab_identifier);
  return tab ? MoveTabToWorkspace(*tab, workspace_identifier)
             : WorkspaceOperationResult{
                   .status = WorkspaceOperationStatus::kStaleTab};
}

WorkspaceOperationResult WorkspaceController::ActivateTabByIdentifier(
    const std::string& window_identifier,
    const std::string& tab_identifier) {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = window_status};
  }
  if (window_identifier != window_identifier_) {
    return {.status = WorkspaceOperationStatus::kForeignWindow};
  }
  std::optional<tabs::TabHandle> tab = FindTabByIdentifier(tab_identifier);
  return tab ? ActivateTab(*tab)
             : WorkspaceOperationResult{
                   .status = WorkspaceOperationStatus::kStaleTab};
}

WorkspaceOperationResult WorkspaceController::CloseTabByIdentifier(
    const std::string& window_identifier,
    const std::string& tab_identifier) {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return {.status = window_status};
  }
  if (window_identifier != window_identifier_) {
    return {.status = WorkspaceOperationStatus::kForeignWindow};
  }
  std::optional<tabs::TabHandle> tab = FindTabByIdentifier(tab_identifier);
  return tab ? CloseTab(*tab)
             : WorkspaceOperationResult{
                   .status = WorkspaceOperationStatus::kStaleTab};
}

std::vector<tabs::TabHandle> WorkspaceController::GetVisibleTabs() const {
  std::vector<tabs::TabHandle> visible;
  if (!tab_list_) {
    return visible;
  }
  for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
    const tabs::TabHandle handle = tab->GetHandle();
    const TabRecord* record = FindTabRecord(handle);
    if (!enabled_ || (record && record->workspace_identifier ==
                                    active_workspace_identifier_)) {
      visible.push_back(handle);
    }
  }
  return visible;
}

bool WorkspaceController::IsTabInActiveWorkspace(tabs::TabHandle tab) const {
  const TabRecord* record = FindTabRecord(tab);
  return !enabled_ || (record && record->workspace_identifier ==
                                     active_workspace_identifier_);
}

std::optional<std::string> WorkspaceController::GetWorkspaceIdentifierForTab(
    tabs::TabHandle tab) const {
  if (!enabled_ || stopped_ || !browser_window_ || !tab_list_) {
    return std::nullopt;
  }
  tabs::TabInterface* tab_interface = tab.Get();
  if (!tab_interface ||
      tab_interface->GetBrowserWindowInterface() != browser_window_ ||
      tab_interface->GetProfile() != profile() ||
      tab_list_->GetIndexOfTab(tab) < 0) {
    return std::nullopt;
  }
  const TabRecord* record = FindTabRecord(tab);
  return record ? std::optional(record->workspace_identifier) : std::nullopt;
}

void WorkspaceController::PersistAll() {
  if (!enabled_ || stopped_ || restoring_ || !browser_window_ ||
      profile()->IsOffTheRecord()) {
    return;
  }
  PersistWindow();
  for (const auto& [raw_handle, record] : tabs_) {
    PersistTab(tabs::TabHandle(raw_handle));
  }
}

void WorkspaceController::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  weak_ptr_factory_.InvalidateWeakPtrs();
  tab_list_observation_.Reset();
  if (browser_window_) {
    GetControllers().erase(browser_window_);
  }
  tab_list_ = nullptr;
  browser_window_ = nullptr;
  tabs_.clear();
}

std::optional<std::string> WorkspaceController::SerializeWindowForSession()
    const {
  if (!enabled_ || stopped_ || restoring_ || !browser_window_ ||
      profile()->IsOffTheRecord() || workspaces_.empty()) {
    return std::nullopt;
  }
  base::DictValue root;
  root.Set("version", kWorkspaceSchemaVersion);
  root.Set("window_id", window_identifier_);
  root.Set("active_workspace_id", active_workspace_identifier_);
  root.Set("default_workspace_id", DefaultWorkspace().identifier);
  base::ListValue workspace_values;
  for (const WorkspaceRecord& workspace : workspaces_) {
    base::DictValue workspace_value;
    workspace_value.Set("id", workspace.identifier);
    workspace_value.Set("name", base::UTF16ToUTF8(workspace.name));
    if (workspace.last_active_tab_identifier) {
      workspace_value.Set("last_active_tab_id",
                          *workspace.last_active_tab_identifier);
    }
    workspace_values.Append(std::move(workspace_value));
  }
  root.Set("workspaces", std::move(workspace_values));
  std::string serialized;
  if (!base::JSONWriter::Write(root, &serialized) ||
      serialized.size() > kMaximumWorkspaceWindowDataBytes) {
    return std::nullopt;
  }
  return serialized;
}

std::optional<std::string> WorkspaceController::SerializeTabForSession(
    tabs::TabHandle tab) const {
  if (!enabled_ || stopped_ || !browser_window_ || restoring_ ||
      profile()->IsOffTheRecord() || !tab_list_) {
    return std::nullopt;
  }
  tabs::TabInterface* tab_interface = tab.Get();
  if (!tab_interface ||
      tab_interface->GetBrowserWindowInterface() != browser_window_ ||
      tab_interface->GetProfile() != profile() ||
      tab_list_->GetIndexOfTab(tab) < 0) {
    return std::nullopt;
  }
  const TabRecord* record = FindTabRecord(tab);
  return record ? SerializeWorkspaceTab(record->identifier,
                                        record->workspace_identifier)
                : std::nullopt;
}

void WorkspaceController::BeginSessionRestore() {
  if (!enabled_ || stopped_ || !browser_window_ ||
      profile()->IsOffTheRecord() || !tab_list_) {
    return;
  }
  restoring_ = true;
  restore_window_envelope_accepted_ = false;
  CreateFreshState();
  for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
    AssignFreshTab(tab, active_workspace_identifier_);
  }
}

bool WorkspaceController::RestoreWindowFromSession(
    const std::string& serialized) {
  restore_window_envelope_accepted_ = false;
  if (!enabled_ || stopped_ || profile()->IsOffTheRecord() ||
      serialized.empty() ||
      serialized.size() > kMaximumWorkspaceWindowDataBytes) {
    return false;
  }
  std::optional<base::Value> value =
      base::JSONReader::Read(serialized, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return false;
  }
  const base::DictValue& root = value->GetDict();
  const std::optional<int> version = root.FindInt("version");
  const std::string* window_identifier = root.FindString("window_id");
  const std::string* active_workspace_identifier =
      root.FindString("active_workspace_id");
  const std::string* default_workspace_identifier =
      root.FindString("default_workspace_id");
  const base::ListValue* workspace_values = root.FindList("workspaces");
  if (version != kWorkspaceSchemaVersion || !window_identifier ||
      !active_workspace_identifier || !default_workspace_identifier ||
      !workspace_values || workspace_values->empty() ||
      workspace_values->size() > kMaximumWorkspaces ||
      !IsCanonicalIdentifier(*window_identifier) ||
      !IsCanonicalIdentifier(*active_workspace_identifier) ||
      !IsCanonicalIdentifier(*default_workspace_identifier)) {
    return false;
  }

  std::vector<WorkspaceRecord> parsed_workspaces;
  std::set<std::string> workspace_identifiers;
  for (const base::Value& workspace_value : *workspace_values) {
    if (!workspace_value.is_dict()) {
      return false;
    }
    const base::DictValue& workspace_dict = workspace_value.GetDict();
    const std::string* identifier = workspace_dict.FindString("id");
    const std::string* utf8_name = workspace_dict.FindString("name");
    const std::string* last_active =
        workspace_dict.FindString("last_active_tab_id");
    if (!identifier || !utf8_name || !base::IsStringUTF8(*utf8_name) ||
        !IsCanonicalIdentifier(*identifier) ||
        !workspace_identifiers.insert(*identifier).second ||
        (last_active && !IsCanonicalIdentifier(*last_active))) {
      return false;
    }
    std::optional<std::u16string> name =
        ValidateWorkspaceName(base::UTF8ToUTF16(*utf8_name));
    if (!name) {
      return false;
    }
    parsed_workspaces.push_back(WorkspaceRecord{
        .identifier = *identifier,
        .name = std::move(*name),
        .last_active_tab_identifier =
            last_active ? std::optional<std::string>(*last_active)
                        : std::nullopt,
        .is_default = *identifier == *default_workspace_identifier,
    });
  }
  if (!workspace_identifiers.contains(*active_workspace_identifier) ||
      std::ranges::count_if(parsed_workspaces, [](const WorkspaceRecord& item) {
        return item.is_default;
      }) != 1) {
    return false;
  }

  for (const auto& [browser_window, controller] : GetControllers()) {
    if (controller != this && controller->profile() == profile() &&
        controller->window_identifier_ == *window_identifier) {
      CreateFreshState();
      for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
        AssignFreshTab(tab, active_workspace_identifier_);
      }
      restoring_ = true;
      return false;
    }
  }

  restoring_ = true;
  restore_window_envelope_accepted_ = true;
  window_identifier_ = *window_identifier;
  active_workspace_identifier_ = *active_workspace_identifier;
  workspaces_ = std::move(parsed_workspaces);
  tabs_.clear();
  for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
    AssignFreshTab(tab, DefaultWorkspace().identifier);
  }
  return true;
}

bool WorkspaceController::RestoreTabFromSession(tabs::TabHandle tab,
                                                const std::string& serialized) {
  if (!enabled_ || stopped_ || !restoring_ ||
      !restore_window_envelope_accepted_ || profile()->IsOffTheRecord() ||
      !tab_list_ || tab_list_->GetIndexOfTab(tab) < 0) {
    return false;
  }
  tabs::TabInterface* tab_interface = tab.Get();
  if (!tab_interface ||
      tab_interface->GetBrowserWindowInterface() != browser_window_ ||
      tab_interface->GetProfile() != profile()) {
    return false;
  }
  std::optional<ParsedWorkspaceTab> parsed = ParseWorkspaceTab(serialized);
  if (!parsed) {
    return false;
  }
  std::string workspace_identifier = parsed->workspace_identifier;
  if (!FindWorkspace(workspace_identifier)) {
    workspace_identifier = DefaultWorkspace().identifier;
  }
  bool duplicate_identifier = false;
  for (const auto& [browser_window, controller] : GetControllers()) {
    if (controller->profile() != profile()) {
      continue;
    }
    duplicate_identifier =
        std::ranges::any_of(controller->tabs_, [&](const auto& item) {
          return (controller != this || item.first != tab.raw_value()) &&
                 item.second.identifier == parsed->tab_identifier;
        });
    if (duplicate_identifier) {
      break;
    }
  }
  tabs_[tab.raw_value()] = TabRecord{
      .identifier =
          duplicate_identifier ? NewIdentifier() : parsed->tab_identifier,
      .workspace_identifier = std::move(workspace_identifier),
      .restored_from_session = true,
  };
  UpdateTabUserData(tab);
  return true;
}

void WorkspaceController::FinalizeSessionRestore() {
  if (!enabled_ || stopped_ || !restoring_ || !tab_list_) {
    return;
  }
  ++mutation_depth_;
  ReconcileAuthoritativeTabs(TabRemovedReason::kDeleted);
  WorkspaceRecord* active_workspace =
      FindWorkspace(active_workspace_identifier_);
  if (!active_workspace) {
    active_workspace_identifier_ = DefaultWorkspace().identifier;
    active_workspace = &DefaultWorkspace();
  }
  tabs::TabInterface* native_active = tab_list_->GetActiveTab();
  TabRecord* native_active_record =
      native_active ? FindTabRecord(native_active->GetHandle()) : nullptr;
  if (native_active_record && native_active_record->restored_from_session &&
      native_active_record->workspace_identifier ==
          active_workspace->identifier) {
    active_workspace->last_active_tab_identifier =
        native_active_record->identifier;
  } else {
    std::optional<tabs::TabHandle> target =
        ResolveWorkspaceActivationTarget(*active_workspace);
    base::WeakPtr<WorkspaceController> alive = weak_ptr_factory_.GetWeakPtr();
    if (target) {
      tab_list_->ActivateTab(*target);
      if (!alive) {
        return;
      }
    } else {
      OpenRealTabInWorkspace(active_workspace->identifier);
      if (!alive) {
        return;
      }
    }
  }
  --mutation_depth_;
  restoring_ = false;
  restore_window_envelope_accepted_ = false;
  UpdateActiveFromChromium();
  for (auto& item : tabs_) {
    item.second.restored_from_session = false;
  }
  PersistAll();
  PublishNow();
}

Profile* WorkspaceController::profile() const {
  return browser_window_ ? browser_window_->GetProfile() : nullptr;
}

WorkspaceController* WorkspaceController::From(
    BrowserWindowInterface* browser_window) {
  auto found = GetControllers().find(browser_window);
  return found == GetControllers().end() ? nullptr : found->second;
}

void WorkspaceController::OnTabAdded(TabListInterface& tab_list,
                                     tabs::TabInterface* tab,
                                     int index) {
  if (!enabled_ || stopped_ || &tab_list != tab_list_ || !tab) {
    SchedulePublish();
    return;
  }
  if (pending_workspace_for_new_tab_) {
    AssignFreshTab(tab, *pending_workspace_for_new_tab_);
  } else {
    AssignTransferredTab(tab);
  }
  if (!restoring_ && tab_list_->GetActiveTab() == tab) {
    UpdateActiveFromChromium();
  }
  PersistTab(tab->GetHandle());
  PersistWindow();
  SchedulePublish();
}

void WorkspaceController::OnActiveTabChanged(TabListInterface& tab_list,
                                             tabs::TabInterface* tab) {
  if (!enabled_ || stopped_ || &tab_list != tab_list_) {
    SchedulePublish();
    return;
  }
  if (tab && !FindTabRecord(tab->GetHandle())) {
    AssignTransferredTab(tab);
  }
  if (!restoring_) {
    UpdateActiveFromChromium();
  }
  PersistWindow();
  SchedulePublish();
}

void WorkspaceController::OnTabRemoved(TabListInterface& tab_list,
                                       tabs::TabInterface* tab,
                                       TabRemovedReason removed_reason) {
  if (!enabled_ || stopped_ || &tab_list != tab_list_) {
    SchedulePublish();
    return;
  }
  ReconcileAuthoritativeTabs(removed_reason);
  if (!restoring_) {
    UpdateActiveFromChromium();
  }
  PersistWindow();
  SchedulePublish();
}

void WorkspaceController::OnTabMoved(TabListInterface& tab_list,
                                     tabs::TabInterface* tab,
                                     int from_index,
                                     int to_index) {
  SchedulePublish();
}

void WorkspaceController::OnWebContentsReplaced(
    TabListInterface& tab_list,
    tabs::TabInterface* tab,
    content::WebContents* old_contents,
    content::WebContents* new_contents) {
  if (enabled_ && !stopped_ && &tab_list == tab_list_ && tab) {
    UpdateTabUserData(tab->GetHandle());
    PersistTab(tab->GetHandle());
  }
  SchedulePublish();
}

void WorkspaceController::OnAllTabsAreClosing(TabListInterface& tab_list) {
  if (&tab_list == tab_list_) {
    closing_all_ = true;
  }
}

void WorkspaceController::OnTabListDestroyed(TabListInterface& tab_list) {
  if (&tab_list != tab_list_) {
    return;
  }
  tab_list_observation_.Reset();
  tab_list_ = nullptr;
  tabs_.clear();
  PublishNow();
}

WorkspaceOperationStatus WorkspaceController::ValidateMutableWindow() const {
  if (!enabled_) {
    return WorkspaceOperationStatus::kPolicyDisabled;
  }
  if (stopped_ || !browser_window_ || browser_window_->IsDeleteScheduled()) {
    return WorkspaceOperationStatus::kWindowUnavailable;
  }
  if (restoring_) {
    return WorkspaceOperationStatus::kSessionRestoreInProgress;
  }
  if (!tab_list_ || tab_list_->IsClosingAllTabs() || closing_all_) {
    return WorkspaceOperationStatus::kTabListUnavailable;
  }
  if (!TabListInterface::CanEditTabList(*profile())) {
    return WorkspaceOperationStatus::kTabListNotEditable;
  }
  return WorkspaceOperationStatus::kSuccess;
}

WorkspaceOperationStatus WorkspaceController::ValidateTab(
    tabs::TabHandle tab) const {
  const WorkspaceOperationStatus window_status = ValidateMutableWindow();
  if (window_status != WorkspaceOperationStatus::kSuccess) {
    return window_status;
  }
  tabs::TabInterface* tab_interface = tab.Get();
  if (!tab_interface) {
    return WorkspaceOperationStatus::kStaleTab;
  }
  if (tab_interface->GetProfile() != profile()) {
    return WorkspaceOperationStatus::kProfileMismatch;
  }
  if (tab_interface->GetBrowserWindowInterface() != browser_window_) {
    return WorkspaceOperationStatus::kForeignWindow;
  }
  if (tab_list_->GetIndexOfTab(tab) < 0) {
    return WorkspaceOperationStatus::kForeignWindow;
  }
  return WorkspaceOperationStatus::kSuccess;
}

WorkspaceController::WorkspaceRecord* WorkspaceController::FindWorkspace(
    const std::string& identifier) {
  auto found =
      std::ranges::find(workspaces_, identifier, &WorkspaceRecord::identifier);
  return found == workspaces_.end() ? nullptr : &*found;
}

const WorkspaceController::WorkspaceRecord* WorkspaceController::FindWorkspace(
    const std::string& identifier) const {
  auto found =
      std::ranges::find(workspaces_, identifier, &WorkspaceRecord::identifier);
  return found == workspaces_.end() ? nullptr : &*found;
}

WorkspaceController::TabRecord* WorkspaceController::FindTabRecord(
    tabs::TabHandle tab) {
  auto found = tabs_.find(tab.raw_value());
  return found == tabs_.end() ? nullptr : &found->second;
}

const WorkspaceController::TabRecord* WorkspaceController::FindTabRecord(
    tabs::TabHandle tab) const {
  auto found = tabs_.find(tab.raw_value());
  return found == tabs_.end() ? nullptr : &found->second;
}

std::optional<tabs::TabHandle> WorkspaceController::FindTabByIdentifier(
    const std::string& tab_identifier) const {
  auto found = std::ranges::find_if(tabs_, [&](const auto& item) {
    return item.second.identifier == tab_identifier;
  });
  return found == tabs_.end()
             ? std::nullopt
             : std::optional<tabs::TabHandle>(tabs::TabHandle(found->first));
}

WorkspaceController::WorkspaceRecord& WorkspaceController::DefaultWorkspace() {
  auto found =
      std::ranges::find(workspaces_, true, &WorkspaceRecord::is_default);
  CHECK(found != workspaces_.end());
  return *found;
}

const WorkspaceController::WorkspaceRecord&
WorkspaceController::DefaultWorkspace() const {
  auto found =
      std::ranges::find(workspaces_, true, &WorkspaceRecord::is_default);
  CHECK(found != workspaces_.end());
  return *found;
}

void WorkspaceController::CreateFreshState() {
  window_identifier_ = NewIdentifier();
  workspaces_.clear();
  workspaces_.push_back(WorkspaceRecord{
      .identifier = NewIdentifier(),
      .name = GetDefaultWorkspaceName(*base::CommandLine::ForCurrentProcess()),
      .is_default = true,
  });
  active_workspace_identifier_ = workspaces_.front().identifier;
  tabs_.clear();
}

void WorkspaceController::AssignFreshTab(
    tabs::TabInterface* tab,
    const std::string& workspace_identifier) {
  CHECK(tab);
  const std::string destination = FindWorkspace(workspace_identifier)
                                      ? workspace_identifier
                                      : DefaultWorkspace().identifier;
  tabs_[tab->GetHandle().raw_value()] = TabRecord{
      .identifier = NewIdentifier(),
      .workspace_identifier = destination,
  };
  UpdateTabUserData(tab->GetHandle());
}

void WorkspaceController::AssignTransferredTab(tabs::TabInterface* tab) {
  CHECK(tab);
  std::string product_tab_identifier;
  if (content::WebContents* contents = tab->GetContents()) {
    if (WorkspaceTabUserData* transferred =
            WorkspaceTabUserData::FromWebContents(contents);
        transferred && IsCanonicalIdentifier(transferred->tab_identifier())) {
      const bool duplicate = std::ranges::any_of(tabs_, [&](const auto& item) {
        return item.second.identifier == transferred->tab_identifier();
      });
      if (!duplicate) {
        product_tab_identifier = transferred->tab_identifier();
      }
    }
  }
  if (product_tab_identifier.empty()) {
    product_tab_identifier = NewIdentifier();
  }
  tabs_[tab->GetHandle().raw_value()] = TabRecord{
      .identifier = std::move(product_tab_identifier),
      .workspace_identifier = active_workspace_identifier_,
  };
  UpdateTabUserData(tab->GetHandle());
}

tabs::TabInterface* WorkspaceController::OpenRealTabInWorkspace(
    const std::string& workspace_identifier) {
  CHECK(FindWorkspace(workspace_identifier));
  CHECK(!pending_workspace_for_new_tab_);
  pending_workspace_for_new_tab_ = workspace_identifier;
  base::WeakPtr<WorkspaceController> alive = weak_ptr_factory_.GetWeakPtr();
  tabs::TabInterface* opened = tab_list_->OpenTab(
      GURL(chrome::kChromeUINewTabURL), tab_list_->GetTabCount(), true);
  if (!alive) {
    return nullptr;
  }
  pending_workspace_for_new_tab_.reset();
  if (!opened) {
    return nullptr;
  }
  TabRecord* record = FindTabRecord(opened->GetHandle());
  if (!record || record->workspace_identifier != workspace_identifier) {
    AssignFreshTab(opened, workspace_identifier);
  }
  active_workspace_identifier_ = workspace_identifier;
  tab_list_->ActivateTab(opened->GetHandle());
  if (!alive) {
    return nullptr;
  }
  UpdateActiveFromChromium();
  return opened;
}

std::optional<tabs::TabHandle>
WorkspaceController::ResolveWorkspaceActivationTarget(
    const WorkspaceRecord& workspace) const {
  std::optional<tabs::TabHandle> first;
  for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
    const tabs::TabHandle handle = tab->GetHandle();
    const TabRecord* record = FindTabRecord(handle);
    if (!record || record->workspace_identifier != workspace.identifier) {
      continue;
    }
    if (!first) {
      first = handle;
    }
    if (workspace.last_active_tab_identifier &&
        record->identifier == *workspace.last_active_tab_identifier) {
      return handle;
    }
  }
  return first;
}

void WorkspaceController::ReconcileAuthoritativeTabs(
    TabRemovedReason removed_reason) {
  if (!tab_list_) {
    tabs_.clear();
    return;
  }
  std::set<int32_t> live_handles;
  for (tabs::TabInterface* tab : tab_list_->GetAllTabs()) {
    live_handles.insert(tab->GetHandle().raw_value());
    if (!FindTabRecord(tab->GetHandle())) {
      AssignTransferredTab(tab);
    }
  }
  std::erase_if(tabs_, [&](const auto& item) {
    return !live_handles.contains(item.first);
  });
  for (WorkspaceRecord& workspace : workspaces_) {
    if (workspace.last_active_tab_identifier &&
        std::ranges::none_of(tabs_, [&](const auto& item) {
          return item.second.identifier ==
                     *workspace.last_active_tab_identifier &&
                 item.second.workspace_identifier == workspace.identifier;
        })) {
      workspace.last_active_tab_identifier.reset();
    }
  }
}

void WorkspaceController::UpdateActiveFromChromium() {
  if (!enabled_ || !tab_list_) {
    return;
  }
  tabs::TabInterface* active = tab_list_->GetActiveTab();
  if (!active) {
    return;
  }
  TabRecord* record = FindTabRecord(active->GetHandle());
  if (!record) {
    AssignTransferredTab(active);
    record = FindTabRecord(active->GetHandle());
  }
  CHECK(record);
  WorkspaceRecord* workspace = FindWorkspace(record->workspace_identifier);
  if (!workspace) {
    record->workspace_identifier = DefaultWorkspace().identifier;
    workspace = &DefaultWorkspace();
    UpdateTabUserData(active->GetHandle());
  }
  active_workspace_identifier_ = workspace->identifier;
  workspace->last_active_tab_identifier = record->identifier;
}

void WorkspaceController::UpdateTabUserData(tabs::TabHandle tab) {
  const TabRecord* record = FindTabRecord(tab);
  tabs::TabInterface* tab_interface = tab.Get();
  if (!record || !tab_interface ||
      tab_interface->GetBrowserWindowInterface() != browser_window_) {
    return;
  }
  content::WebContents* contents = tab_interface->GetContents();
  if (!contents) {
    return;
  }
  WorkspaceTabUserData::CreateForWebContents(contents, record->identifier,
                                             record->workspace_identifier);
  WorkspaceTabUserData::FromWebContents(contents)->Update(
      record->identifier, record->workspace_identifier);
}

void WorkspaceController::PersistWindow() {
  if (restoring_ || persistence_.persist_window.is_null()) {
    return;
  }
  if (std::optional<std::string> serialized = SerializeWindowForSession()) {
    persistence_.persist_window.Run(browser_window_, *serialized);
  }
}

void WorkspaceController::PersistTab(tabs::TabHandle tab) {
  if (restoring_ || persistence_.persist_tab.is_null()) {
    return;
  }
  if (std::optional<std::string> serialized = SerializeTabForSession(tab)) {
    persistence_.persist_tab.Run(browser_window_, tab, *serialized);
  }
}

void WorkspaceController::SchedulePublish() {
  if (!ShouldPublish() || publish_scheduled_) {
    return;
  }
  publish_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<WorkspaceController> controller) {
                       if (!controller) {
                         return;
                       }
                       controller->publish_scheduled_ = false;
                       controller->PublishNow();
                     },
                     weak_ptr_factory_.GetWeakPtr()));
}

void WorkspaceController::PublishNow() {
  if (!ShouldPublish()) {
    return;
  }
  publish_scheduled_ = false;
  state_changed_.Run();
}

bool WorkspaceController::ShouldPublish() const {
  return !stopped_ && !restoring_ && mutation_depth_ == 0 &&
         !state_changed_.is_null();
}

base::WeakPtr<WorkspaceController> WorkspaceController::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

std::optional<std::string> GetWorkspaceWindowSessionData(
    BrowserWindowInterface* browser_window) {
  WorkspaceController* controller = WorkspaceController::From(browser_window);
  return controller ? controller->SerializeWindowForSession() : std::nullopt;
}

std::optional<std::string> GetWorkspaceTabSessionData(
    content::WebContents* contents) {
  if (!contents) {
    return std::nullopt;
  }
  tabs::TabInterface* tab = tabs::TabInterface::GetFromContents(contents);
  if (!tab) {
    return std::nullopt;
  }
  WorkspaceController* controller =
      WorkspaceController::From(tab->GetBrowserWindowInterface());
  return controller ? controller->SerializeTabForSession(tab->GetHandle())
                    : std::nullopt;
}

void RestoreWorkspaceWindowSessionData(
    BrowserWindowInterface* browser_window,
    const std::map<std::string, std::string>& extra_data) {
  WorkspaceController* controller = WorkspaceController::From(browser_window);
  if (controller) {
    controller->BeginSessionRestore();
  }
  auto found = extra_data.find(kWorkspaceWindowSessionKey);
  if (controller && found != extra_data.end()) {
    controller->RestoreWindowFromSession(found->second);
  }
}

void RestoreWorkspaceTabSessionData(
    BrowserWindowInterface* browser_window,
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data) {
  WorkspaceController* controller = WorkspaceController::From(browser_window);
  auto found = extra_data.find(kWorkspaceTabSessionKey);
  if (!controller || !contents || found == extra_data.end()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabInterface::GetFromContents(contents);
  if (tab && tab->GetBrowserWindowInterface() == browser_window) {
    controller->RestoreTabFromSession(tab->GetHandle(), found->second);
  }
}

void FinalizeWorkspaceSessionRestore(Profile* profile) {
  std::vector<base::WeakPtr<WorkspaceController>> matching;
  for (const auto& [browser_window, controller] : GetControllers()) {
    if (controller->profile() == profile) {
      matching.push_back(controller->GetWeakPtr());
    }
  }
  for (base::WeakPtr<WorkspaceController>& controller : matching) {
    if (controller) {
      controller->FinalizeSessionRestore();
    }
  }
}

}  // namespace limitless
