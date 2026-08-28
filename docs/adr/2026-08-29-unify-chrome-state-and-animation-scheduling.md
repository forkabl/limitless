# Unify Chrome State and Animation Scheduling

## Status

Accepted

## Problem

Limitless embeds SwiftUI and AppKit inside Chromium's macOS event loop. External
actions could mutate Swift state without reliably advancing SwiftUI's AppKit
update phases. Command-S exposed this when the sidebar waited for another app
event before rendering.

Geometry animations also resized every mounted `WebContents` on every frame.
Sidebar and panel transitions therefore competed with Chromium layout and
compositing work. Forced redraws and redraw polling treated symptoms while
adding more frame work.

## Decision

Use three shared mechanisms:

1. The Objective-C++ bridge claims external chrome actions synchronously, then
   performs Swift mutations on AppKit's main queue.
2. `ChromeTransitionDriver` owns geometry-changing transitions, post-mutation
   SwiftUI notification, reduced motion, and a bounded AppKit wake clock for
   shortcut-driven native animations and final viewport settlement. The clock
   advances normal update phases without forcing redraws.
3. `WebContentsResizeCoordinator` holds the active Chromium viewport at stable
   geometry, animates its parent layer, and commits one final engine resize.

SwiftUI describes settled chrome state. Hidden Chromium views retain their last
size and synchronize lazily when selected. Scoped SwiftUI animations remain
appropriate for native-only micro-interactions. Snapshots are a fallback for
surfaces that cannot animate reliably as live layers.

## Rollout

- Route keyboard and bridge actions through the shared main-queue and update
  scheduling path.
- Remove forced redraws and redraw polling.
- Use the shared transition path for sidebar, sidebar position, AI panel,
  extension panel, and future geometry-changing chrome.
- Verify web-focused shortcuts, repeated transitions, heavy pages, multiple
  realized tabs, reduced motion, and final viewport sizing.

## Consequences

- External actions are claimed before Chromium and mutate Swift state on the
  AppKit main queue.
- Chromium avoids frame-by-frame resize work during chrome transitions.
- The browser remains live during animation and resizes once when settled.
- Animation behavior is centralized without coupling product state to redraw
  hacks.
