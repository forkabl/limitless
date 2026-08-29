// Seam between LimitlessBrowserWindow / chrome internals and the Limitless bridge
// (limitless_chrome_bridge.mm). ObjC++-includable; pure-C++ callers use only the
// non-ObjC declarations.

#ifndef CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_CHROME_HOOKS_H_
#define CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_CHROME_HOOKS_H_

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#endif

class Browser;

namespace limitless {

// First normal Browser becomes the Limitless browser; the SwiftUI chrome attaches
// to it. Called from LimitlessBrowserWindow's ctor/dtor and Show().
void OnBrowserWindowCreated(Browser* browser);
void OnBrowserWindowDestroyed(Browser* browser);
void EnsureLimitlessUIStarted(Browser* browser);

// The Browser instance the Limitless UI drives (null before startup).
Browser* LimitlessBrowser();
void PrepareLimitlessBrowserForDestruction(Browser* browser);

#ifdef __OBJC__
// Claims an NSEvent synchronously and schedules its Swift action on AppKit's
// main queue. Returns false when the event belongs to Chromium or the page.
bool RouteChromeShortcut(NSEvent* event);

// The shared Limitless main window (used for GetNativeWindow / dialog parenting).
NSWindow* LimitlessMainWindow();
#endif

}  // namespace limitless

#endif  // CHROME_BROWSER_UI_LIMITLESS_LIMITLESS_CHROME_HOOKS_H_
