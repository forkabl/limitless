#ifndef CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_PERMISSION_PROMPT_H_
#define CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_PERMISSION_PROMPT_H_

#include <memory>

#include "components/permissions/permission_prompt.h"

namespace content {
class WebContents;
}

namespace limitless {

// A native sheet on the Limitless window standing in for Chrome's views
// permission bubble (which needs the toolbar we replaced).
std::unique_ptr<permissions::PermissionPrompt> CreateLimitlessPermissionPrompt(
    content::WebContents* web_contents,
    permissions::PermissionPrompt::Delegate* delegate);

}  // namespace limitless

#endif  // CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_PERMISSION_PROMPT_H_
