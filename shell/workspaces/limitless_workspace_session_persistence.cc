// Copyright 2026 The Limitless Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "limitless_workspace_session_persistence.h"

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace limitless {
namespace {

void PersistWorkspaceWindow(BrowserWindowInterface* browser_window,
                            const std::string& serialized) {
  if (!browser_window || browser_window->IsDeleteScheduled() ||
      browser_window->GetProfile()->IsOffTheRecord()) {
    return;
  }
  SessionService* service =
      SessionServiceFactory::GetForProfile(browser_window->GetProfile());
  if (service) {
    service->AddWindowExtraData(browser_window->GetSessionID(),
                                kWorkspaceWindowSessionKey, serialized);
  }
}

void PersistWorkspaceTab(BrowserWindowInterface* browser_window,
                         tabs::TabHandle handle,
                         const std::string& serialized) {
  if (!browser_window || browser_window->IsDeleteScheduled() ||
      browser_window->GetProfile()->IsOffTheRecord()) {
    return;
  }
  tabs::TabInterface* tab = handle.Get();
  if (!tab || tab->GetBrowserWindowInterface() != browser_window ||
      tab->GetProfile() != browser_window->GetProfile()) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  sessions::SessionTabHelper* session_tab_helper =
      contents ? sessions::SessionTabHelper::FromWebContents(contents)
               : nullptr;
  SessionService* service =
      SessionServiceFactory::GetForProfile(browser_window->GetProfile());
  if (service && session_tab_helper) {
    service->AddTabExtraData(browser_window->GetSessionID(),
                             session_tab_helper->session_id(),
                             kWorkspaceTabSessionKey, serialized);
  }
}

void EnsureSessionRestoreObservation() {
  static base::NoDestructor<base::CallbackListSubscription> subscription(
      SessionRestore::RegisterOnSessionRestoredCallback(
          base::BindRepeating([](Profile* profile, int restored_tab_count) {
            FinalizeWorkspaceSessionRestore(profile);
          })));
}

}  // namespace

WorkspacePersistenceCallbacks CreateWorkspaceSessionPersistence(
    Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return {};
  }
  EnsureSessionRestoreObservation();
  return WorkspacePersistenceCallbacks{
      .persist_window = base::BindRepeating(&PersistWorkspaceWindow),
      .persist_tab = base::BindRepeating(&PersistWorkspaceTab),
  };
}

}  // namespace limitless
