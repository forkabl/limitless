// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_CONTROLLER_H_
#define LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/tab_list/tab_list_interface_observer.h"
#include "components/tabs/public/tab_interface.h"

class BrowserWindowInterface;
class Profile;
class TabListInterface;

namespace content {
class WebContents;
}

namespace limitless {

inline constexpr char kWorkspacesSwitch[] = "limitless-workspaces";
inline constexpr char kDefaultWorkspaceNameSwitch[] =
    "limitless-default-workspace-name";
inline constexpr char kWorkspaceWindowSessionKey[] =
    "limitless.workspaces.window.v1";
inline constexpr char kWorkspaceTabSessionKey[] = "limitless.workspaces.tab.v1";

bool AreWorkspacesEnabled(const base::CommandLine& command_line);
std::u16string GetDefaultWorkspaceName(const base::CommandLine& command_line);

enum class WorkspaceOperationStatus {
  kSuccess,
  kPolicyDisabled,
  kWindowUnavailable,
  kTabListUnavailable,
  kSessionRestoreInProgress,
  kInvalidName,
  kWorkspaceLimitReached,
  kUnknownWorkspace,
  kStaleTab,
  kForeignWindow,
  kProfileMismatch,
  kTabListNotEditable,
  kUnsplitFailed,
  kOpenTabFailed,
};

struct WorkspaceOperationResult {
  WorkspaceOperationStatus status =
      WorkspaceOperationStatus::kWindowUnavailable;
  std::optional<std::string> workspace_identifier;

  bool succeeded() const {
    return status == WorkspaceOperationStatus::kSuccess;
  }
};

struct WorkspaceProjection {
  std::string identifier;
  std::u16string name;
  size_t order = 0;
  bool is_default = false;
};

struct WorkspaceTabProjection {
  std::string identifier;
  tabs::TabHandle handle;
};

struct WorkspaceWindowSnapshot {
  bool enabled = false;
  bool session_restore_in_progress = false;
  std::string window_identifier;
  std::vector<WorkspaceProjection> workspaces;
  std::optional<std::string> active_workspace_identifier;
  std::vector<tabs::TabHandle> visible_tabs;
  std::vector<WorkspaceTabProjection> visible_tab_projections;
  std::optional<tabs::TabHandle> active_tab;
};

struct WorkspacePersistenceCallbacks {
  base::RepeatingCallback<void(BrowserWindowInterface*, const std::string&)>
      persist_window;
  base::RepeatingCallback<
      void(BrowserWindowInterface*, tabs::TabHandle, const std::string&)>
      persist_tab;
};

class WorkspaceController final : public TabListInterfaceObserver {
 public:
  WorkspaceController(BrowserWindowInterface* browser_window,
                      WorkspacePersistenceCallbacks persistence,
                      base::RepeatingClosure state_changed);
  ~WorkspaceController() override;

  WorkspaceController(const WorkspaceController&) = delete;
  WorkspaceController& operator=(const WorkspaceController&) = delete;

  WorkspaceWindowSnapshot GetSnapshot() const;
  WorkspaceOperationResult CreateWorkspace(const std::u16string& name);
  WorkspaceOperationResult SwitchWorkspace(
      const std::string& workspace_identifier);
  WorkspaceOperationResult MoveTabToWorkspace(
      tabs::TabHandle tab,
      const std::string& workspace_identifier);
  WorkspaceOperationResult ActivateTab(tabs::TabHandle tab);
  WorkspaceOperationResult CloseTab(tabs::TabHandle tab);
  WorkspaceOperationResult MoveTabToWorkspaceByIdentifier(
      const std::string& window_identifier,
      const std::string& tab_identifier,
      const std::string& workspace_identifier);
  WorkspaceOperationResult ActivateTabByIdentifier(
      const std::string& window_identifier,
      const std::string& tab_identifier);
  WorkspaceOperationResult CloseTabByIdentifier(
      const std::string& window_identifier,
      const std::string& tab_identifier);

  std::vector<tabs::TabHandle> GetVisibleTabs() const;
  bool IsTabInActiveWorkspace(tabs::TabHandle tab) const;
  std::optional<std::string> GetWorkspaceIdentifierForTab(
      tabs::TabHandle tab) const;
  void PersistAll();
  void Stop();

  std::optional<std::string> SerializeWindowForSession() const;
  std::optional<std::string> SerializeTabForSession(tabs::TabHandle tab) const;
  void BeginSessionRestore();
  bool RestoreWindowFromSession(const std::string& serialized);
  bool RestoreTabFromSession(tabs::TabHandle tab,
                             const std::string& serialized);
  void FinalizeSessionRestore();

  BrowserWindowInterface* browser_window() const { return browser_window_; }
  Profile* profile() const;

  static WorkspaceController* From(BrowserWindowInterface* browser_window);

  void OnTabAdded(TabListInterface& tab_list,
                  tabs::TabInterface* tab,
                  int index) override;
  void OnActiveTabChanged(TabListInterface& tab_list,
                          tabs::TabInterface* tab) override;
  void OnTabRemoved(TabListInterface& tab_list,
                    tabs::TabInterface* tab,
                    TabRemovedReason removed_reason) override;
  void OnTabMoved(TabListInterface& tab_list,
                  tabs::TabInterface* tab,
                  int from_index,
                  int to_index) override;
  void OnWebContentsReplaced(TabListInterface& tab_list,
                             tabs::TabInterface* tab,
                             content::WebContents* old_contents,
                             content::WebContents* new_contents) override;
  void OnAllTabsAreClosing(TabListInterface& tab_list) override;
  void OnTabListDestroyed(TabListInterface& tab_list) override;

 private:
  friend void FinalizeWorkspaceSessionRestore(Profile* profile);

  struct WorkspaceRecord {
    std::string identifier;
    std::u16string name;
    std::optional<std::string> last_active_tab_identifier;
    bool is_default = false;
  };

  struct TabRecord {
    std::string identifier;
    std::string workspace_identifier;
    // Ephemeral restore-epoch provenance. This is never serialized and only
    // prevents a startup placeholder without accepted tab metadata from
    // overriding a restored workspace's saved last-active member.
    bool restored_from_session = false;
  };

  WorkspaceOperationStatus ValidateMutableWindow() const;
  WorkspaceOperationStatus ValidateTab(tabs::TabHandle tab) const;
  WorkspaceRecord* FindWorkspace(const std::string& identifier);
  const WorkspaceRecord* FindWorkspace(const std::string& identifier) const;
  TabRecord* FindTabRecord(tabs::TabHandle tab);
  const TabRecord* FindTabRecord(tabs::TabHandle tab) const;
  std::optional<tabs::TabHandle> FindTabByIdentifier(
      const std::string& tab_identifier) const;
  WorkspaceRecord& DefaultWorkspace();
  const WorkspaceRecord& DefaultWorkspace() const;
  void CreateFreshState();
  void AssignFreshTab(tabs::TabInterface* tab,
                      const std::string& workspace_identifier);
  void AssignTransferredTab(tabs::TabInterface* tab);
  tabs::TabInterface* OpenRealTabInWorkspace(
      const std::string& workspace_identifier);
  std::optional<tabs::TabHandle> ResolveWorkspaceActivationTarget(
      const WorkspaceRecord& workspace) const;
  void ReconcileAuthoritativeTabs(TabRemovedReason removed_reason);
  void UpdateActiveFromChromium();
  void UpdateTabUserData(tabs::TabHandle tab);
  void PersistWindow();
  void PersistTab(tabs::TabHandle tab);
  void SchedulePublish();
  void PublishNow();
  bool ShouldPublish() const;
  base::WeakPtr<WorkspaceController> GetWeakPtrForSessionRestore();

  bool enabled_ = false;
  bool closing_all_ = false;
  bool stopped_ = false;
  bool restoring_ = false;
  bool restore_window_envelope_accepted_ = false;
  int mutation_depth_ = 0;
  bool publish_scheduled_ = false;
  std::string window_identifier_;
  std::string active_workspace_identifier_;
  std::optional<std::string> pending_workspace_for_new_tab_;
  raw_ptr<BrowserWindowInterface> browser_window_;
  raw_ptr<TabListInterface> tab_list_;
  WorkspacePersistenceCallbacks persistence_;
  base::RepeatingClosure state_changed_;
  std::vector<WorkspaceRecord> workspaces_;
  std::map<int32_t, TabRecord> tabs_;
  base::ScopedObservation<TabListInterface, TabListInterfaceObserver>
      tab_list_observation_{this};
  base::WeakPtrFactory<WorkspaceController> weak_ptr_factory_{this};
};

std::optional<std::string> GetWorkspaceWindowSessionData(
    BrowserWindowInterface* browser_window);
std::optional<std::string> GetWorkspaceTabSessionData(
    content::WebContents* contents);
void RestoreWorkspaceWindowSessionData(
    BrowserWindowInterface* browser_window,
    const std::map<std::string, std::string>& extra_data);
void RestoreWorkspaceTabSessionData(
    BrowserWindowInterface* browser_window,
    content::WebContents* contents,
    const std::map<std::string, std::string>& extra_data);
void FinalizeWorkspaceSessionRestore(Profile* profile);

}  // namespace limitless

#endif  // LIMITLESS_SHELL_WORKSPACES_LIMITLESS_WORKSPACE_CONTROLLER_H_
