# Make Embedded Chromium Resizing Presentation-Paced

## Status

Proposed

## Context

Limitless lays out native SwiftUI and AppKit chrome around a live Chromium
`WebContents` view. Two common interactions change the space available to that
view:

1. The user resizes the native window interactively.
2. Limitless changes its own chrome geometry, such as toggling the sidebar with
   Command-S.

The existing implementation uses different resize policies for these cases.
During a normal window resize, `WebContainerView.ContainerView` forwards each
AppKit layout update directly to the visible `LimitlessBrowserView`. The native
Chromium child has a width-and-height autoresizing mask, so each forwarded size
can trigger Chromium visual-property synchronization, renderer layout, raster,
and compositing work.

During a finite chrome transition, `WebContentsResizeCoordinator` holds the
Chromium view at its previous native size and applies a Core Animation transform
to its parent layer. At transition completion, `WebContainerView` applies the
final frame and calls `commitViewportLayout`. The Objective-C++ bridge then
forces `layoutSubtreeIfNeeded` on both the wrapper and Chromium's native child.

This design avoids resizing Chromium on every frame of a sidebar animation, but
it still has two user-visible failure modes:

- Interactive window resizing is paced by AppKit and SwiftUI layout callbacks,
  not by the display or Chromium's ability to present a correctly sized frame.
  Complex pages fall behind, and the visible browser surface advances in large,
  irregular steps.
- Finite chrome transitions end by removing the presentation transform and
  forcing a synchronous native layout. The old viewport therefore changes to
  the final viewport as one visible jump, and a complex page can make the tail
  of the transition pause while it reflows.

An end-to-end reproduction used a separate test profile, native macOS window
sizing, a native Command-S event, and Playwright instrumentation inside the
selected page. The results were:

- A 31-step native window resize at approximately 60 Hz produced only 10
  distinct viewport widths on a complex Chrome Web Store page. Gaps between
  distinct viewport updates reached 215 ms.
- The same resize on a lightweight page produced 30 distinct viewport widths,
  with gaps between 18 ms and 37 ms. This isolates page and Chromium resize work
  as the main amplifier rather than the native window driver.
- A Command-S sidebar transition held the page at 631 points, changed it once to
  964 points at settlement, and was followed by an approximately 167 ms page
  responsiveness gap on the complex page.
- Transition logging showed that shortcut routing and animation scheduling did
  run: the sidebar transition began and ended in approximately 186 ms. The hitch
  was at viewport settlement, not shortcut delivery.

The current `chrome_animation_contract_test.sh` verifies source structure. It
does not exercise native window live resize, presentation cadence, or the final
Chromium surface handoff. DevTools `Browser.setWindowBounds` cannot substitute
for a native resize because `LimitlessBrowserWindow::SetBounds` is intentionally
a no-op.

This ADR extends and replaces the viewport-resize details in
`2026-08-29-unify-chrome-state-and-animation-scheduling.md`. The earlier
decision to centralize chrome transitions and avoid frame-by-frame forced
redraws remains valid.

## Decision

Make `WebContentsResizeCoordinator` the single owner of visible Chromium
viewport sizing and presentation. AppKit and SwiftUI will report desired
geometry, but they will not directly force the Chromium subtree to lay out.

The coordinator will separate requested geometry, committed native geometry,
and presented Chromium geometry. It will use an explicit state machine with the
following conceptual states:

- `idle`: requested, committed, and presented sizes agree.
- `interactiveResize`: the native window is in live resize and target geometry
  may change continuously.
- `chromeTransition`: a finite Limitless chrome animation is changing the
  container while Chromium remains at stable committed geometry.
- `awaitingPresentation`: the final native size has been committed and the old
  presentation remains visible until Chromium acknowledges a frame for the new
  viewport generation.

Every resize request will carry a monotonically increasing generation. Stale
commit or presentation callbacks must not release a newer resize operation.

### Interactive native window resize

`ContainerView` will use AppKit's live-resize lifecycle to enter and leave
interactive resize. Size changes received while live resize is active will be
coalesced and committed at most once per display refresh through a display-link
driven scheduler.

The scheduler will always keep only the newest target size. It must not queue a
backlog of intermediate sizes. A slow page may skip renderer viewport sizes,
but it must not stall native chrome movement or replay stale sizes after the
pointer has moved on.

Until Chromium presents a frame matching the latest committed surface
generation, the coordinator will keep the last complete surface visible and
use compositor-only positioning, clipping, and bounded scaling to cover the
current container. Chromium surface synchronization, rather than synchronous
AppKit subtree layout, will determine when the new surface replaces the old
presentation.

When live resize ends, the coordinator will flush the newest target exactly
once, enter `awaitingPresentation`, and release temporary presentation geometry
only after the matching Chromium frame is ready. A bounded recovery timeout may
release the hold if Chromium is shutting down or no presentation callback can
arrive, but the timeout is a safety path rather than normal pacing.

### Finite chrome geometry transitions

Sidebar, panel, sidebar-position, and similar animations will continue to hold
Chromium at stable committed geometry while SwiftUI animates the surrounding
chrome. The current compositor-transform approach remains the normal
presentation path during the animation.

At logical animation completion, the coordinator will:

1. Commit only the latest settled viewport size to the active Chromium view.
2. Keep the old transformed presentation visible.
3. Wait for Chromium to acknowledge a presented frame for that viewport
   generation.
4. Remove the transform and reveal the correctly sized live surface in one Core
   Animation transaction.

The implementation must not clear the transform before the final surface is
ready. It must also handle an interrupted or immediately reversed transition by
invalidating the older generation and continuing from the currently visible
presentation without flashing an intermediate size.

### Chromium bridge contract

Replace `commitViewportLayout` with an asynchronous viewport commit contract.
Setting the wrapper and native child bounds will occur with implicit Core
Animation actions disabled, but it will not call `layoutSubtreeIfNeeded` from a
SwiftUI update, AppKit resize callback, or transition-completion callback.

The Objective-C++ bridge will expose a presentation acknowledgement tied to the
committed viewport generation and size. The implementation should use the
pinned Chromium revision's compositor or `RenderWidgetHostView` presentation
callback rather than infer readiness from a timer. The Swift layer does not need
to know Chromium surface identifiers, but the bridge must reject an
acknowledgement for the wrong size or an obsolete generation.

Only visible `WebContents` participate in resize pacing. Hidden tabs retain
their previous size and synchronize when selected, preserving the existing
background-tab policy. Split view commits both visible panes as one generation
so one pane cannot reveal a new surface while the other still shows old
geometry.

### View ownership and scheduling

`WebContainerView.ContainerView` will stop assigning browser subview frames
independently from both `layout()` and `setFrameSize()`. Those callbacks may
report new container bounds to the coordinator, but the coordinator alone will
decide whether to update presentation geometry, commit native geometry, or do
nothing because a newer request is already pending.

`ChromeTransitionDriver` remains responsible for SwiftUI chrome animation
lifecycle. It will not be responsible for pacing Chromium resizes. Its wake
clock may advance SwiftUI transactions, but resize correctness and final surface
release must not depend on synthetic AppKit events or a guessed animation
duration.

## Behavioral verification

Add a repeatable end-to-end resize test that launches a separate Limitless test
profile and serves deterministic lightweight and complex local fixtures.
Playwright CLI will instrument the selected page and assert final viewport
geometry. A small native macOS test driver will use accessibility APIs to resize
the actual Limitless window and post Command-S, since CDP cannot exercise the
native window or shortcut path.

The test will record native target sizes, coordinator commits, Chromium
presentation acknowledgements, page viewport changes, and frame timestamps.
The performance log format must include a resize generation so these events can
be correlated without relying on wall-clock ordering alone.

On the supported 60 Hz development configuration, the following are acceptance
criteria:

- Native chrome presentation has no gap longer than 50 ms during either a
  500 ms interactive window resize or a Command-S sidebar transition on the
  complex fixture.
- Browser presentation work is committed at most once per display refresh, and
  no obsolete target size is committed after a newer target.
- A finite chrome transition performs one terminal viewport commit per visible
  pane.
- The final page viewport matches the settled browser-card size within one
  point and is presented within two display frames of Chromium's matching frame
  acknowledgement.
- Consecutive captured frames contain no blank sliver, stale untransformed edge,
  or geometry discontinuity larger than one point at handoff.
- The reduced-motion path reaches the same final geometry without an animated
  hold and without synchronous subtree layout.
- Repeated Command-S toggles, transition reversal, window resize during a chrome
  transition, split view, multiple realized tabs, renderer crash, and window
  close do not leave the coordinator holding geometry.

The structural contract test will remain as a fast guard, but it is not evidence
that resize behavior is smooth.

## Rollout

1. Add generation-based resize instrumentation and the failing native-window
   plus Playwright end-to-end test.
2. Route all visible browser frame requests through
   `WebContentsResizeCoordinator` and remove duplicate direct frame assignment.
3. Add display-link coalescing and AppKit live-resize lifecycle handling.
4. Add the Chromium presentation acknowledgement to the bridge.
5. Replace synchronous `commitViewportLayout` settlement with asynchronous
   commit-and-handoff for finite chrome transitions.
6. Validate the acceptance matrix on lightweight, complex, media, canvas, and
   split-view pages before removing the old settlement path.

## Alternatives considered

### Continue resizing Chromium on every native layout callback

Rejected. It is simple, but a complex page makes renderer and compositor work
part of the perceived drag cadence. AppKit event coalescing cannot provide a
stable presentation contract.

### Keep the current frozen transform and synchronous final commit

Rejected. It reduces work during the middle of a finite transition but exposes
the most expensive resize as a visible terminal jump. Forced subtree layout also
couples SwiftUI animation completion to Chromium native layout cost.

### Debounce until the interaction ends

Rejected. It avoids repeated renderer work but leaves the browser visibly stale
for the entire drag and provides no frame-ready handoff at release.

### Use a static bitmap snapshot for every resize

Rejected as the primary path. It cannot faithfully represent video, animated
canvas, protected content, cursor changes, or continuously updating pages. A
snapshot may remain a bounded recovery fallback if Chromium cannot preserve its
last compositor surface, but it is not the normal architecture.

### Tune the SwiftUI animation or AppKit wake timer

Rejected. The reproduction shows that shortcut delivery occurs and that page
complexity changes the hitch by an order of magnitude. Timing changes do not
remove synchronous layout or establish a presented-surface handoff.

## Consequences

- Native chrome and pointer tracking remain responsive even when a page cannot
  reflow at display cadence.
- Window resizing and Limitless chrome transitions share one geometry and
  presentation model instead of using unrelated policies.
- The final browser surface appears only when Chromium has actually presented
  the requested size, eliminating the forced-layout tail hitch and blank-edge
  races.
- Resize behavior gains a more complex state machine and a new Objective-C++
  presentation callback. Generation checks, interruption tests, and teardown
  handling are therefore required for correctness.
- A slow page may temporarily show bounded compositor scaling while a newer
  surface is pending. This is preferable to blocking native interaction, and
  the handoff criteria limit how long that state can persist during normal
  operation.
