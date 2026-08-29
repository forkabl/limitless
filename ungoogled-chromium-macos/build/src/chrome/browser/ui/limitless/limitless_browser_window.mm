// Limitless BrowserWindow: mostly inert (SwiftUI owns the chrome); window-level
// queries answer against the shared Limitless NSWindow.

#include "chrome/browser/ui/limitless/limitless_browser_window.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "base/strings/sys_string_conversions.h"
#include "chrome/browser/share/share_attempt.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/autofill/autofill_bubble_handler.h"
#include "chrome/browser/ui/autofill/save_address_bubble_controller.h"
#include "chrome/browser/ui/autofill/update_address_bubble_controller.h"
#include "chrome/browser/ui/limitless/limitless_chrome_hooks.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_bubble_type.h"
#include "chrome/browser/ui/views/bubble_anchor_util_views.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/sharing_message/sharing_dialog_data.h"
#include "components/web_modal/modal_dialog_host.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/gfx/range/range.h"

// Swift-exported surface of LimitlessRoot (see LimitlessRoot.swift).
@interface LimitlessRoot : NSObject
+ (void)toggleBookmarkForURL:(NSString*)url title:(NSString*)title;
+ (void)shareURL:(NSString*)url title:(NSString*)title;
+ (void)showQRCodeForURL:(NSString*)url title:(NSString*)title;
+ (void)translateURL:(NSString*)url;
+ (void)translateText:(NSString*)text;
+ (void)showNativeNotice:(NSString*)message icon:(NSString*)icon;
+ (void)showTabSearch;
+ (void)focusOmnibox;
@end

namespace {

// Limitless' chrome shortcuts (⌘S toggle sidebar, ⌘T toggle omnibox, …) belong to
// the SwiftUI registry. Claim them here - in the browser's keyboard pre-handler
// - so they win *before* the focused web page or Chromium's own commands
// (Save Page As on ⌘S, New Tab on ⌘T) can act. This is what makes the
// shortcuts fire reliably while web content has focus, instead of racing the
// app-level NSEvent monitor and Chromium's native accelerators (the
// intermittent "needs two presses" behavior).
bool HandleLimitlessShortcut(const input::NativeWebKeyboardEvent& event) {
  // Only fire on the raw key-down; ignore synthesized char and key-up events.
  if (event.GetType() != input::NativeWebKeyboardEvent::Type::kRawKeyDown) {
    return false;
  }
  NSEvent* ns_event = event.os_event.Get();
  if (!ns_event || ns_event.type != NSEventTypeKeyDown) {
    return false;
  }
  return limitless::RouteChromeShortcut(ns_event);
}

NSString* OriginDisclosureLabel(const url::Origin& origin) {
  const std::string serialized_origin = origin.Serialize();
  if (serialized_origin.empty() || serialized_origin == "null") {
    return @"Limitless Browser";
  }
  return base::SysUTF8ToNSString(serialized_origin);
}

void ConfigureDisclosureLabel(NSTextField* label,
                              NSFont* font,
                              NSColor* color) {
  label.font = font;
  label.textColor = color;
  label.backgroundColor = NSColor.clearColor;
  label.bordered = NO;
  label.editable = NO;
  label.selectable = NO;
  label.lineBreakMode = NSLineBreakByTruncatingMiddle;
}

class LimitlessAutofillBubbleHandler final : public autofill::AutofillBubbleHandler {
 public:
  LimitlessAutofillBubbleHandler() = default;
  ~LimitlessAutofillBubbleHandler() override = default;

  autofill::AutofillBubbleBase* ShowSaveCreditCardBubble(
      content::WebContents* web_contents,
      autofill::SaveCardBubbleController* controller,
      bool is_user_gesture) override {
    Notice(@"Payment autofill bubbles are not exposed in Limitless yet.",
           @"creditcard");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowIbanBubble(
      content::WebContents* web_contents,
      autofill::IbanBubbleController* controller,
      bool is_user_gesture,
      autofill::IbanBubbleType bubble_type) override {
    Notice(@"Payment autofill bubbles are not exposed in Limitless yet.",
           @"creditcard");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowOfferNotificationBubble(
      content::WebContents* web_contents,
      autofill::OfferNotificationBubbleController* controller,
      bool is_user_gesture) override {
    Notice(@"Autofill offer bubbles are not exposed in Limitless yet.", @"tag");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowSaveAutofillAiDataBubble(
      content::WebContents* web_contents,
      autofill::AutofillAiImportDataController* controller) override {
    Notice(@"Autofill AI bubbles are not exposed in Limitless yet.", @"sparkles");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowAutofillAiLocalSaveNotification(
      content::WebContents* web_contents,
      autofill::AutofillAiImportDataController* controller) override {
    Notice(@"Autofill AI bubbles are not exposed in Limitless yet.", @"sparkles");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowSaveAddressProfileBubble(
      content::WebContents* web_contents,
      std::unique_ptr<autofill::SaveAddressBubbleController> controller,
      bool is_user_gesture) override {
    Notice(@"Address autofill bubbles are not exposed in Limitless yet.",
           @"person.text.rectangle");
    return nullptr;
  }

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  autofill::AutofillBubbleBase* ShowAddressSignInPromo(
      content::WebContents* web_contents,
      const autofill::AutofillProfile& autofill_profile) override {
    Notice(@"Address autofill sign-in is not exposed in Limitless yet.",
           @"person.crop.circle.badge.plus");
    return nullptr;
  }
#endif

  autofill::AutofillBubbleBase* ShowUpdateAddressProfileBubble(
      content::WebContents* web_contents,
      std::unique_ptr<autofill::UpdateAddressBubbleController> controller,
      bool is_user_gesture) override {
    Notice(@"Address autofill bubbles are not exposed in Limitless yet.",
           @"person.text.rectangle");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowFilledCardInformationBubble(
      content::WebContents* web_contents,
      autofill::FilledCardInformationBubbleController* controller,
      bool is_user_gesture) override {
    Notice(@"Payment autofill bubbles are not exposed in Limitless yet.",
           @"creditcard");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowVirtualCardEnrollBubble(
      content::WebContents* web_contents,
      autofill::VirtualCardEnrollBubbleController* controller,
      bool is_user_gesture) override {
    Notice(@"Virtual card enrollment is not exposed in Limitless yet.",
           @"creditcard.trianglebadge.exclamationmark");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowVirtualCardEnrollConfirmationBubble(
      content::WebContents* web_contents,
      autofill::VirtualCardEnrollBubbleController* controller) override {
    Notice(@"Virtual card enrollment is not exposed in Limitless yet.",
           @"creditcard.trianglebadge.exclamationmark");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowMandatoryReauthBubble(
      content::WebContents* web_contents,
      autofill::MandatoryReauthBubbleController* controller,
      bool is_user_gesture,
      autofill::MandatoryReauthBubbleType bubble_type) override {
    Notice(@"Autofill reauthentication is not exposed in Limitless yet.",
           @"lock.shield");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowSaveCardConfirmationBubble(
      content::WebContents* web_contents,
      autofill::SaveCardBubbleController* controller) override {
    Notice(@"Payment autofill bubbles are not exposed in Limitless yet.",
           @"creditcard");
    return nullptr;
  }

  autofill::AutofillBubbleBase* ShowSaveIbanConfirmationBubble(
      content::WebContents* web_contents,
      autofill::IbanBubbleController* controller) override {
    Notice(@"Payment autofill bubbles are not exposed in Limitless yet.",
           @"creditcard");
    return nullptr;
  }

 private:
  static void Notice(NSString* message, NSString* icon) {
    [LimitlessRoot showNativeNotice:message icon:icon];
  }
};

}  // namespace

LimitlessBrowserWindow::LimitlessBrowserWindow(Browser* browser)
    : browser_(browser),
      modal_dialog_host_(),
      exclusive_access_context_(browser),
      location_bar_(browser) {
  limitless::OnBrowserWindowCreated(browser);
}

// --- LimitlessFindBar --------------------------------------------------------------

FindBarController* LimitlessFindBar::GetFindBarController() const {
  return nullptr;
}

void LimitlessFindBar::SetFindBarController(FindBarController* find_bar_controller) {}

void LimitlessFindBar::Show(bool animate, bool focus) {}

void LimitlessFindBar::Hide(bool animate) {}

void LimitlessFindBar::SetFocusAndSelection() {}

void LimitlessFindBar::ClearResults( const find_in_page::FindNotificationDetails& results) {}

void LimitlessFindBar::StopAnimation() {}

void LimitlessFindBar::MoveWindowIfNecessary() {}

void LimitlessFindBar::SetFindTextAndSelectedRange( const std::u16string& find_text, const gfx::Range& selected_range) {}

std::u16string_view LimitlessFindBar::GetFindText() const {
  return {};
}

gfx::Range LimitlessFindBar::GetSelectedRange() const {
  return {};
}

void LimitlessFindBar::UpdateUIForFindResult( const find_in_page::FindNotificationDetails& result, const std::u16string& find_text) {}

void LimitlessFindBar::AudibleAlert() {}

bool LimitlessFindBar::IsFindBarVisible() const {
  return false;
}

void LimitlessFindBar::RestoreSavedFocus() {}

bool LimitlessFindBar::HasGlobalFindPasteboard() const {
  return false;
}

void LimitlessFindBar::UpdateFindBarForChangedWebContents() {}

bool LimitlessFindBar::CanPopulateFromSelectedText() {
  return false;
}

const FindBarTesting* LimitlessFindBar::GetFindBarTesting() const {
  return nullptr;
}

bool LimitlessFindBar::HasFocus() const {
  return false;
}

void LimitlessFindBar::CloseOverlappingBubbles() {}

views::Widget* LimitlessFindBar::GetHostWidget() {
  return nullptr;
}

// --- LimitlessLocationBar ---------------------------------------------------------

void LimitlessLocationBar::FocusLocation(bool is_user_initiated, bool clear_focus_if_failed) {}

void LimitlessLocationBar::FocusSearch() {}

void LimitlessLocationBar::UpdateFocusBehavior(bool toolbar_visible) {}

void LimitlessLocationBar::UpdateContentSettingsIcons() {}

void LimitlessLocationBar::SaveStateToContents(content::WebContents* contents) {}

void LimitlessLocationBar::Revert() {}

OmniboxView* LimitlessLocationBar::GetOmniboxView() {
  return nullptr;
}

OmniboxPopupView* LimitlessLocationBar::GetOmniboxPopupView() {
  return nullptr;
}

OmniboxController* LimitlessLocationBar::GetOmniboxController() {
  return nullptr;
}

bool LimitlessLocationBar::ShouldCloseOmniboxPopup(ui::MouseEvent* event) {
  return false;
}

content::WebContents* LimitlessLocationBar::GetWebContents() {
  return nullptr;
}

LocationBarModel* LimitlessLocationBar::GetLocationBarModel() {
  return nullptr;
}

std::optional<bubble_anchor_util::AnchorConfiguration> LimitlessLocationBar::GetChipAnchor() {
  return {};
}

ChipController* LimitlessLocationBar::GetChipController() {
  return nullptr;
}

void LimitlessLocationBar::OnChanged() {}

void LimitlessLocationBar::UpdateWithoutTabRestore() {}

ui::TrackedElement* LimitlessLocationBar::GetAnchorOrNull() {
  return nullptr;
}

Browser* LimitlessLocationBar::GetBrowser() {
  return browser_;
}

Profile* LimitlessLocationBar::GetProfile() {
  return browser_ ? browser_->profile() : nullptr;
}

bool LimitlessLocationBar::IsInitialized() const {
  return false;
}

bool LimitlessLocationBar::IsVisible() const {
  return false;
}

bool LimitlessLocationBar::IsDrawn() const {
  return false;
}

bool LimitlessLocationBar::IsFullscreen() const {
  return false;
}

bool LimitlessLocationBar::IsEditingOrEmpty() const {
  return false;
}

void LimitlessLocationBar::InvalidateLayout() {}

gfx::Rect LimitlessLocationBar::Bounds() const {
  return {};
}

gfx::Rect LimitlessLocationBar::BoundsInScreen() const {
  return {};
}

gfx::Size LimitlessLocationBar::MinimumSize() const {
  return {};
}

gfx::Size LimitlessLocationBar::PreferredSize() const {
  return {};
}

void LimitlessLocationBar::Update(content::WebContents* contents) {}

void LimitlessLocationBar::ResetTabState(content::WebContents* contents) {}

bool LimitlessLocationBar::HasSecurityStateChanged() {
  return false;
}

LocationBarTesting* LimitlessLocationBar::GetLocationBarForTesting() {
  return nullptr;
}

// --- LimitlessExclusiveAccessContext ---------------------------------------------

LimitlessExclusiveAccessContext::LimitlessExclusiveAccessContext(Browser* browser)
    : browser_(browser) {}

LimitlessExclusiveAccessContext::~LimitlessExclusiveAccessContext() {
  HideFullscreenDisclosure(ExclusiveAccessBubbleHideReason::kInterrupted);
}

Profile* LimitlessExclusiveAccessContext::GetProfile() {
  return browser_->profile();
}

bool LimitlessExclusiveAccessContext::IsFullscreen() const {
  NSWindow* window = limitless::LimitlessMainWindow();
  return window && (window.styleMask & NSWindowStyleMaskFullScreen);
}

void LimitlessExclusiveAccessContext::EnterFullscreen(
    const url::Origin& origin,
    ExclusiveAccessBubbleType bubble_type,
    FullscreenTabParams fullscreen_tab_params) {
  NSWindow* window = limitless::LimitlessMainWindow();
  if (window && !(window.styleMask & NSWindowStyleMaskFullScreen)) {
    [window toggleFullScreen:nil];
  }
  ShowFullscreenDisclosure(origin);
}

void LimitlessExclusiveAccessContext::ExitFullscreen() {
  HideFullscreenDisclosure(ExclusiveAccessBubbleHideReason::kInterrupted);
  NSWindow* window = limitless::LimitlessMainWindow();
  if (window && (window.styleMask & NSWindowStyleMaskFullScreen)) {
    [window toggleFullScreen:nil];
  }
}

void LimitlessExclusiveAccessContext::UpdateExclusiveAccessBubble(
    const ExclusiveAccessBubbleParams& params,
    ExclusiveAccessBubbleHideCallback first_hide_callback) {
  const bool should_close_bubble =
      !params.has_download &&
      params.type == EXCLUSIVE_ACCESS_BUBBLE_TYPE_NONE;
  if (should_close_bubble) {
    if (first_hide_callback) {
      std::move(first_hide_callback)
          .Run(ExclusiveAccessBubbleHideReason::kNotShown);
    }
    HideFullscreenDisclosure(ExclusiveAccessBubbleHideReason::kInterrupted);
    return;
  }
  ShowFullscreenDisclosure(params.origin, std::move(first_hide_callback));
}

bool LimitlessExclusiveAccessContext::IsExclusiveAccessBubbleDisplayed() const {
  return exclusive_access_bubble_visible_;
}

void LimitlessExclusiveAccessContext::OnExclusiveAccessUserInput() {
  NSPanel* panel = (__bridge NSPanel*)fullscreen_disclosure_.get();
  if (panel) {
    [panel orderFront:nil];
  }
}

content::WebContents* LimitlessExclusiveAccessContext::GetWebContentsForExclusiveAccess() {
  return browser_->tab_strip_model()->GetActiveWebContents();
}

bool LimitlessExclusiveAccessContext::CanUserEnterFullscreen() const {
  return true;
}

bool LimitlessExclusiveAccessContext::CanUserExitFullscreen() const {
  return true;
}

void LimitlessExclusiveAccessContext::ShowFullscreenDisclosure(
    const url::Origin& origin,
    ExclusiveAccessBubbleHideCallback first_hide_callback) {
  NSWindow* parent = limitless::LimitlessMainWindow();
  if (!parent) {
    if (first_hide_callback) {
      std::move(first_hide_callback)
          .Run(ExclusiveAccessBubbleHideReason::kNotShown);
    }
    return;
  }

  HideFullscreenDisclosure(ExclusiveAccessBubbleHideReason::kInterrupted);

  const NSRect parent_frame = parent.frame;
  const CGFloat width = std::min<CGFloat>(
      520.0, std::max<CGFloat>(320.0, parent_frame.size.width - 48.0));
  const CGFloat height = 72.0;
  const NSRect frame = NSMakeRect(NSMidX(parent_frame) - width / 2.0,
                                  NSMaxY(parent_frame) - height - 28.0,
                                  width, height);

  NSPanel* panel =
      [[NSPanel alloc] initWithContentRect:frame
                                 styleMask:NSWindowStyleMaskBorderless |
                                           NSWindowStyleMaskNonactivatingPanel
                                   backing:NSBackingStoreBuffered
                                     defer:NO];
  panel.opaque = NO;
  panel.backgroundColor = NSColor.clearColor;
  panel.hasShadow = YES;
  panel.ignoresMouseEvents = YES;
  panel.level = parent.level + 1;
  panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary |
                             NSWindowCollectionBehaviorCanJoinAllSpaces |
                             NSWindowCollectionBehaviorTransient;

  NSView* container =
      [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
  container.wantsLayer = YES;
  container.layer.cornerRadius = 14.0;
  container.layer.masksToBounds = YES;
  container.layer.backgroundColor =
      [[NSColor colorWithCalibratedWhite:0.06 alpha:0.86] CGColor];

  NSTextField* title = [NSTextField labelWithString:@"Full screen"];
  title.frame = NSMakeRect(20.0, 45.0, width - 40.0, 18.0);
  ConfigureDisclosureLabel(
      title, [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold],
      NSColor.whiteColor);

  NSTextField* origin_label =
      [NSTextField labelWithString:OriginDisclosureLabel(origin)];
  origin_label.frame = NSMakeRect(20.0, 26.0, width - 40.0, 16.0);
  ConfigureDisclosureLabel(origin_label,
                           [NSFont systemFontOfSize:12.0
                                             weight:NSFontWeightRegular],
                           [NSColor colorWithCalibratedWhite:1.0 alpha:0.78]);

  NSTextField* instruction =
      [NSTextField labelWithString:@"Press Esc to exit full screen"];
  instruction.frame = NSMakeRect(20.0, 10.0, width - 40.0, 15.0);
  ConfigureDisclosureLabel(instruction,
                           [NSFont systemFontOfSize:11.0
                                             weight:NSFontWeightRegular],
                           [NSColor colorWithCalibratedWhite:1.0 alpha:0.58]);

  [container addSubview:title];
  [container addSubview:origin_label];
  [container addSubview:instruction];
  panel.contentView = container;

  [parent addChildWindow:panel ordered:NSWindowAbove];
  [panel orderFront:nil];

  fullscreen_disclosure_ = (__bridge_retained void*)panel;
  fullscreen_disclosure_hide_callback_ = std::move(first_hide_callback);
  exclusive_access_bubble_visible_ = true;
}

void LimitlessExclusiveAccessContext::HideFullscreenDisclosure(
    ExclusiveAccessBubbleHideReason reason) {
  NSPanel* panel =
      (__bridge_transfer NSPanel*)fullscreen_disclosure_.get();
  fullscreen_disclosure_ = nullptr;
  exclusive_access_bubble_visible_ = false;

  if (panel) {
    [panel.parentWindow removeChildWindow:panel];
    [panel orderOut:nil];
    [panel close];
  }

  if (fullscreen_disclosure_hide_callback_) {
    std::move(fullscreen_disclosure_hide_callback_).Run(reason);
  }
}

LimitlessBrowserWindow::~LimitlessBrowserWindow() {
  limitless::OnBrowserWindowDestroyed(browser_);
}

// --- LimitlessModalDialogHost ----------------------------------------------------

LimitlessModalDialogHost::LimitlessModalDialogHost() = default;

LimitlessModalDialogHost::~LimitlessModalDialogHost() {
  for (auto& observer : observers_) {
    observer.OnHostDestroying();
  }
}

gfx::NativeView LimitlessModalDialogHost::GetHostView() const {
  return gfx::NativeView(limitless::LimitlessMainWindow().contentView);
}

gfx::Point LimitlessModalDialogHost::GetDialogPosition(const gfx::Size& size) {
  NSView* content = limitless::LimitlessMainWindow().contentView;
  const int width = content ? NSWidth(content.bounds) : 1280;
  return gfx::Point(std::max(0, (width - size.width()) / 2), 64);
}

gfx::Size LimitlessModalDialogHost::GetMaximumDialogSize() {
  NSView* content = limitless::LimitlessMainWindow().contentView;
  if (!content) {
    return gfx::Size(1200, 760);
  }
  return gfx::Size(NSWidth(content.bounds), NSHeight(content.bounds));
}

void LimitlessModalDialogHost::AddObserver(
    web_modal::ModalDialogHostObserver* observer) {
  observers_.AddObserver(observer);
}

void LimitlessModalDialogHost::RemoveObserver(
    web_modal::ModalDialogHostObserver* observer) {
  observers_.RemoveObserver(observer);
}

bool LimitlessBrowserWindow::IsMaximized() const {
  return false;
}

bool LimitlessBrowserWindow::IsMinimized() const {
  return false;
}

bool LimitlessBrowserWindow::IsFullscreen() const {
  return false;
}

void LimitlessBrowserWindow::Hide() {}

void LimitlessBrowserWindow::ShowInactive() {}

void LimitlessBrowserWindow::Deactivate() {}

void LimitlessBrowserWindow::Maximize() {}

void LimitlessBrowserWindow::Minimize() {}

void LimitlessBrowserWindow::Restore() {}

void LimitlessBrowserWindow::FlashFrame(bool flash) {}

ui::ZOrderLevel LimitlessBrowserWindow::GetZOrderLevel() const {
  return ui::ZOrderLevel::kNormal;
}

void LimitlessBrowserWindow::SetZOrderLevel(ui::ZOrderLevel order) {}

bool LimitlessBrowserWindow::IsOnCurrentWorkspace() const {
  return false;
}

bool LimitlessBrowserWindow::IsVisibleOnScreen() const {
  return false;
}

void LimitlessBrowserWindow::SetTopControlsShownRatio(content::WebContents* web_contents, float ratio) {}

bool LimitlessBrowserWindow::DoBrowserControlsShrinkRendererSize( const content::WebContents* contents) const {
  return false;
}

ui::NativeTheme* LimitlessBrowserWindow::GetNativeTheme() {
  return nullptr;
}

const ui::ThemeProvider* LimitlessBrowserWindow::GetThemeProvider() const {
  return nullptr;
}

const ui::ColorProvider* LimitlessBrowserWindow::GetColorProvider() const {
  return nullptr;
}

int LimitlessBrowserWindow::GetTopControlsHeight() const {
  return {};
}

void LimitlessBrowserWindow::SetTopControlsGestureScrollInProgress(bool in_progress) {}

std::vector<StatusBubble*> LimitlessBrowserWindow::GetStatusBubbles() {
  return {};
}

void LimitlessBrowserWindow::UpdateTitleBar() {}

void LimitlessBrowserWindow::BookmarkBarStateChanged( BookmarkBar::AnimateChangeType change_type) {}

void LimitlessBrowserWindow::TemporarilyShowBookmarkBar(base::TimeDelta duration) {}

void LimitlessBrowserWindow::UpdateDevTools(content::WebContents* inspected_web_contents) {}

bool LimitlessBrowserWindow::CanDockDevTools() const {
  return false;
}

void LimitlessBrowserWindow::UpdateLoadingAnimations(bool is_visible) {}

void LimitlessBrowserWindow::SetStarredState(bool is_starred) {}

bool LimitlessBrowserWindow::IsTabModalPopupDeprecated() const {
  return false;
}

void LimitlessBrowserWindow::SetIsTabModalPopupDeprecated( bool is_tab_modal_popup_deprecated) {}

void LimitlessBrowserWindow::OnActiveTabChanged(content::WebContents* old_contents, content::WebContents* new_contents, int index, int reason) {}

void LimitlessBrowserWindow::OnTabDetached(content::WebContents* contents, bool was_active) {}

void LimitlessBrowserWindow::ZoomChangedForActiveTab(bool can_show_bubble) {}

bool LimitlessBrowserWindow::ShouldHideUIForFullscreen() const {
  return false;
}

bool LimitlessBrowserWindow::IsFullscreenBubbleVisible() const {
  return false;
}

bool LimitlessBrowserWindow::IsForceFullscreen() const {
  return false;
}

void LimitlessBrowserWindow::SetForceFullscreen(bool force_fullscreen) {}

gfx::Size LimitlessBrowserWindow::GetContentsSize() const {
  return {};
}

void LimitlessBrowserWindow::SetContentsSize(const gfx::Size& size) {}

void LimitlessBrowserWindow::UpdatePageActionIcon(PageActionIconType type) {}

autofill::AutofillBubbleHandler* LimitlessBrowserWindow::GetAutofillBubbleHandler() {
  static LimitlessAutofillBubbleHandler* handler = new LimitlessAutofillBubbleHandler();
  return handler;
}

void LimitlessBrowserWindow::ExecutePageActionIconForTesting(PageActionIconType type) {}

LocationBar* LimitlessBrowserWindow::GetLocationBar() const {
  return const_cast<LimitlessLocationBar*>(&location_bar_);
}

void LimitlessBrowserWindow::SetFocusToLocationBar(bool is_user_initiated) {
  [LimitlessRoot focusOmnibox];
}

void LimitlessBrowserWindow::UpdateReloadStopState(bool is_loading, bool force) {}

void LimitlessBrowserWindow::UpdateToolbar(content::WebContents* contents) {}

bool LimitlessBrowserWindow::UpdateToolbarSecurityState() {
  return false;
}

void LimitlessBrowserWindow::UpdateCustomTabBarVisibility(bool visible, bool animate) {}

void LimitlessBrowserWindow::SetDevToolsScrimVisibility(bool visible) {}

void LimitlessBrowserWindow::ResetToolbarTabState(content::WebContents* contents) {}

void LimitlessBrowserWindow::FocusToolbar() {}

void LimitlessBrowserWindow::ToolbarSizeChanged(bool is_animating) {}

void LimitlessBrowserWindow::TabDraggingStatusChanged(bool is_dragging) {}

void LimitlessBrowserWindow::LinkOpeningFromGesture(WindowOpenDisposition disposition) {}

void LimitlessBrowserWindow::FocusAppMenu() {}

void LimitlessBrowserWindow::FocusBookmarksToolbar() {}

void LimitlessBrowserWindow::FocusInactivePopupForAccessibility() {}

void LimitlessBrowserWindow::RotatePaneFocus(bool forwards) {}

void LimitlessBrowserWindow::FocusWebContentsPane() {
  if (content::WebContents* contents =
          browser_->tab_strip_model()->GetActiveWebContents()) {
    contents->Focus();
  }
}

bool LimitlessBrowserWindow::IsBookmarkBarVisible() const {
  return false;
}

bool LimitlessBrowserWindow::IsBookmarkBarAnimating() const {
  return false;
}

bool LimitlessBrowserWindow::IsTabStripEditable() const {
  return true;
}

void LimitlessBrowserWindow::DisableTabStripEditingForTesting() {}

bool LimitlessBrowserWindow::IsToolbarVisible() const {
  return false;
}

bool LimitlessBrowserWindow::IsToolbarShowing() const {
  return false;
}

bool LimitlessBrowserWindow::IsLocationBarVisible() const {
  return false;
}

SharingDialog* LimitlessBrowserWindow::ShowSharingDialog(content::WebContents* contents, SharingDialogData data) {
  content::WebContents* target =
      contents ? contents : browser_->tab_strip_model()->GetActiveWebContents();
  if (target) {
    [LimitlessRoot shareURL:base::SysUTF8ToNSString(target->GetVisibleURL().spec())
                 title:base::SysUTF16ToNSString(target->GetTitle())];
  }
  return nullptr;
}

void LimitlessBrowserWindow::ShowUpdateChromeDialog() {}

void LimitlessBrowserWindow::ShowIntentPickerBubble( std::vector<apps::IntentPickerAppInfo> app_info, bool show_stay_in_chrome, bool show_remember_selection, apps::IntentPickerBubbleType bubble_type, const std::optional<url::Origin>& initiating_origin, IntentPickerResponse callback) {
  if (app_info.empty()) {
    std::move(callback).Run(std::string(), apps::PickerEntryType::kUnknown,
                            apps::IntentPickerCloseReason::STAY_IN_CHROME,
                            false);
    return;
  }

  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Open this link in another app?";
  alert.informativeText = @"Choose an app or keep browsing in Limitless.";
  alert.alertStyle = NSAlertStyleInformational;
  for (const auto& app : app_info) {
    [alert addButtonWithTitle:base::SysUTF8ToNSString(app.display_name)];
  }
  [alert addButtonWithTitle:@"Stay in Limitless"];

  NSModalResponse response = [alert runModal];
  NSInteger selected = response - NSAlertFirstButtonReturn;
  if (selected >= 0 && selected < static_cast<NSInteger>(app_info.size())) {
    const auto& app = app_info[static_cast<size_t>(selected)];
    std::move(callback).Run(app.launch_name, app.type,
                            apps::IntentPickerCloseReason::OPEN_APP,
                            false);
    return;
  }
  std::move(callback).Run(std::string(), apps::PickerEntryType::kUnknown,
                          apps::IntentPickerCloseReason::STAY_IN_CHROME,
                          false);
}

void LimitlessBrowserWindow::ShowBookmarkBubble(const GURL& url, bool already_bookmarked) {
  const std::string spec = url.is_valid() ? url.spec() : std::string();
  NSString* title = @"";
  if (content::WebContents* contents =
          browser_->tab_strip_model()->GetActiveWebContents()) {
    title = base::SysUTF16ToNSString(contents->GetTitle());
  }
  [LimitlessRoot toggleBookmarkForURL:base::SysUTF8ToNSString(spec) title:title];
}

sharing_hub::ScreenshotCapturedBubble* LimitlessBrowserWindow::ShowScreenshotCapturedBubble( content::WebContents* contents, const gfx::Image& image) {
  [LimitlessRoot showNativeNotice:@"Screenshot captured."
                        icon:@"camera.viewfinder"];
  return nullptr;
}

qrcode_generator::QRCodeGeneratorBubbleView* LimitlessBrowserWindow::ShowQRCodeGeneratorBubble(content::WebContents* contents, const GURL& url, bool show_back_button) {
  NSString* title = contents ? base::SysUTF16ToNSString(contents->GetTitle()) : @"";
  const std::string spec = url.is_valid()
                               ? url.spec()
                               : (contents ? contents->GetVisibleURL().spec()
                                           : std::string());
  [LimitlessRoot showQRCodeForURL:base::SysUTF8ToNSString(spec) title:title];
  return nullptr;
}

send_tab_to_self::SendTabToSelfBubbleView* LimitlessBrowserWindow::ShowSendTabToSelfDevicePickerBubble(content::WebContents* contents) {
  [LimitlessRoot showNativeNotice:@"Send to device is not exposed in Limitless yet."
                        icon:@"paperplane"];
  return nullptr;
}

send_tab_to_self::SendTabToSelfBubbleView* LimitlessBrowserWindow::ShowSendTabToSelfPromoBubble(content::WebContents* contents, bool show_signin_button) {
  [LimitlessRoot showNativeNotice:@"Send to device is not exposed in Limitless yet."
                        icon:@"paperplane"];
  return nullptr;
}

sharing_hub::SharingHubBubbleView* LimitlessBrowserWindow::ShowSharingHubBubble( share::ShareAttempt attempt) {
  if (content::WebContents* contents =
          browser_->tab_strip_model()->GetActiveWebContents()) {
    [LimitlessRoot shareURL:base::SysUTF8ToNSString(contents->GetVisibleURL().spec())
                 title:base::SysUTF16ToNSString(contents->GetTitle())];
  }
  return nullptr;
}

ShowTranslateBubbleResult LimitlessBrowserWindow::ShowTranslateBubble( content::WebContents* contents, translate::TranslateStep step, const std::string& source_language, const std::string& target_language, translate::TranslateErrors error_type, bool is_user_gesture) {
  if (contents) {
    [LimitlessRoot translateURL:base::SysUTF8ToNSString(contents->GetVisibleURL().spec())];
  }
  return {};
}

void LimitlessBrowserWindow::StartPartialTranslate(const std::string& source_language, const std::string& target_language, const std::u16string& text_selection) {
  [LimitlessRoot translateText:base::SysUTF16ToNSString(text_selection)];
}

DownloadBubbleUIController* LimitlessBrowserWindow::GetDownloadBubbleUIController() {
  return nullptr;
}

void LimitlessBrowserWindow::ConfirmBrowserCloseWithPendingDownloads( int download_count, Browser::DownloadCloseType dialog_type, base::OnceCallback<void(bool)> callback) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = download_count == 1
                          ? @"A download is still in progress."
                          : [NSString stringWithFormat:@"%d downloads are still in progress.",
                                                       download_count];
  alert.informativeText = @"Closing Limitless now will cancel unfinished downloads.";
  alert.alertStyle = NSAlertStyleWarning;
  [alert addButtonWithTitle:@"Close Anyway"];
  [alert addButtonWithTitle:@"Keep Browsing"];
  NSModalResponse response = [alert runModal];
  std::move(callback).Run(response == NSAlertFirstButtonReturn);
}

void LimitlessBrowserWindow::ShowAppMenu() {
  NSMenu* appMenu = [[NSApp.mainMenu itemAtIndex:0] submenu];
  if (!appMenu) {
    return;
  }
  NSWindow* window = limitless::LimitlessMainWindow();
  NSPoint point = window ? NSMakePoint(NSMidX(window.frame), NSMaxY(window.frame) - 40)
                         : NSMakePoint(24, 24);
  [appMenu popUpMenuPositioningItem:nil atLocation:point inView:nil];
}

void LimitlessBrowserWindow::PreHandleDragUpdate(const content::DropData& drop_data, const gfx::PointF& point) {}

void LimitlessBrowserWindow::PreHandleDragExit() {}

void LimitlessBrowserWindow::HandleDragEnded() {}

content::KeyboardEventProcessingResult LimitlessBrowserWindow::PreHandleKeyboardEvent( const input::NativeWebKeyboardEvent& event) {
  if (HandleLimitlessShortcut(event)) {
    return content::KeyboardEventProcessingResult::HANDLED;
  }
  return content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool LimitlessBrowserWindow::HandleKeyboardEvent( const input::NativeWebKeyboardEvent& event) {
  return false;
}

std::unique_ptr<FindBar> LimitlessBrowserWindow::CreateFindBar() {
  return std::make_unique<LimitlessFindBar>();
}

web_modal::WebContentsModalDialogHost*
LimitlessBrowserWindow::GetWebContentsModalDialogHost() {
  return &modal_dialog_host_;
}

web_modal::WebContentsModalDialogHost*
LimitlessBrowserWindow::GetWebContentsModalDialogHostFor(
    content::WebContents* web_contents) {
  return &modal_dialog_host_;
}

void LimitlessBrowserWindow::ShowAvatarBubbleFromAvatarButton(bool is_source_accelerator) {
  [LimitlessRoot showNativeNotice:@"Profiles are not exposed in Limitless yet."
                        icon:@"person.crop.circle"];
}

void LimitlessBrowserWindow::MaybeShowProfileSwitchIPH() {}

void LimitlessBrowserWindow::MaybeShowSupervisedUserProfileSignInIPH() {}

void LimitlessBrowserWindow::ShowHatsDialog( const std::string& site_id, const std::optional<std::string>& hats_histogram_name, const std::optional<uint64_t> hats_survey_ukm_id, base::OnceClosure success_callback, base::OnceClosure failure_callback, const SurveyBitsData& product_specific_bits_data, const SurveyStringData& product_specific_string_data) {}

ExclusiveAccessContext* LimitlessBrowserWindow::GetExclusiveAccessContext() {
  return &exclusive_access_context_;
}

std::string LimitlessBrowserWindow::GetWorkspace() const {
  return {};
}

bool LimitlessBrowserWindow::IsVisibleOnAllWorkspaces() const {
  return false;
}

void LimitlessBrowserWindow::ShowEmojiPanel() {
  [NSApp orderFrontCharacterPalette:nil];
}

std::unique_ptr<content::EyeDropper> LimitlessBrowserWindow::OpenEyeDropper( content::RenderFrameHost* frame, content::EyeDropperListener* listener) {
  [LimitlessRoot showNativeNotice:@"Eye dropper is not exposed in Limitless yet."
                        icon:@"eyedropper"];
  return {};
}

void LimitlessBrowserWindow::ShowCaretBrowsingDialog() {
  [LimitlessRoot showNativeNotice:@"Caret browsing is not exposed in Limitless yet."
                        icon:@"text.cursor"];
}

void LimitlessBrowserWindow::CreateTabSearchBubble() {
  [LimitlessRoot showTabSearch];
}

void LimitlessBrowserWindow::CloseTabSearchBubble() {}

void LimitlessBrowserWindow::ShowIncognitoClearBrowsingDataDialog() {
  [LimitlessRoot showNativeNotice:@"Private browsing is not exposed in Limitless yet."
                        icon:@"eye.slash"];
}

void LimitlessBrowserWindow::ShowIncognitoHistoryDisclaimerDialog() {
  [LimitlessRoot showNativeNotice:@"Private browsing is not exposed in Limitless yet."
                        icon:@"eye.slash"];
}

bool LimitlessBrowserWindow::IsUnframedModeEnabled() const {
  return false;
}

bool LimitlessBrowserWindow::GetCanResize() {
  return false;
}

ui::mojom::WindowShowState LimitlessBrowserWindow::GetWindowShowState() const {
  return {};
}

void LimitlessBrowserWindow::ShowChromeLabs() {
  [LimitlessRoot showNativeNotice:@"Chrome Labs is not exposed in Limitless."
                        icon:@"flask"];
}

BrowserView* LimitlessBrowserWindow::AsBrowserView() {
  return nullptr;
}

void LimitlessBrowserWindow::DeleteBrowserWindow() {
  delete this;
}

// --- Real implementations against the shared Limitless window -------------------

namespace {
NSWindow* LimitlessWindow() {
  return limitless::LimitlessMainWindow();
}
}  // namespace

void LimitlessBrowserWindow::Show() {
  limitless::EnsureLimitlessUIStarted(browser_);
}

void LimitlessBrowserWindow::Close() {
  // Match BrowserView's close protocol: let Browser own tab shutdown and
  // asynchronous deletion. HandleBeforeClose() may pause here while a page
  // runs beforeunload, then re-enter Close() once that work is complete.
  if (!browser_->HandleBeforeClose()) {
    return;
  }

  // OnWindowClosing() closes a non-empty tab strip and TabStripEmpty()
  // re-enters this method. Tear down BrowserWindowFeatures only on that final
  // pass, before Browser schedules its asynchronous destruction task.
  if (browser_->tab_strip_model()->empty() && !features_torn_down_) {
    features_torn_down_ = true;
    // LimitlessBrowserView retains native WebContents until SwiftUI releases
    // its tab models. Clear their Browser delegate before the Browser-owned
    // controllers are destroyed so late renderer close acknowledgements no-op.
    limitless::PrepareLimitlessBrowserForDestruction(browser_);
    browser_->GetFeatures().TearDownPreBrowserWindowDestruction();
  }

  browser_->OnWindowClosing();
}

bool LimitlessBrowserWindow::IsActive() const {
  return LimitlessWindow().isKeyWindow;
}

void LimitlessBrowserWindow::Activate() {
  [LimitlessWindow() makeKeyAndOrderFront:nil];
}

gfx::NativeWindow LimitlessBrowserWindow::GetNativeWindow() const {
  return gfx::NativeWindow(LimitlessWindow());
}

gfx::Rect LimitlessBrowserWindow::GetBounds() const {
  NSWindow* window = LimitlessWindow();
  if (!window) {
    return gfx::Rect(0, 0, 1280, 820);
  }
  NSRect f = window.frame;
  NSScreen* screen = window.screen ?: NSScreen.screens.firstObject;
  const CGFloat flipped_y = NSMaxY(screen.frame) - NSMaxY(f);
  return gfx::Rect(NSMinX(f), flipped_y, NSWidth(f), NSHeight(f));
}

gfx::Rect LimitlessBrowserWindow::GetRestoredBounds() const {
  return GetBounds();
}

ui::mojom::WindowShowState LimitlessBrowserWindow::GetRestoredState() const {
  return ui::mojom::WindowShowState::kNormal;
}

bool LimitlessBrowserWindow::IsVisible() const {
  return LimitlessWindow().isVisible;
}

void LimitlessBrowserWindow::SetBounds(const gfx::Rect& bounds) {}
