# Make Embedded Chromium Resizing Commit- and Presentation-Paced

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

The pinned Chromium revision already owns important parts of this pipeline:

- `RenderWidgetHostViewMac` forwards native size changes to
  `BrowserCompositorMac`, which allocates `LocalSurfaceId` values and embeds the
  renderer surface through `DelegatedFrameHost`.
- `DelegatedFrameHost` owns fallback surfaces and resize deadline policy.
- `RenderWidgetHostImpl` can throttle visual-property updates to one renderer
  commit at a time. The macOS resize path currently bypasses that throttle by
  default because `kThrottleResizeIpc` is disabled.
- `RenderWidgetHostView::SetShouldUseDefaultDeadlineOnResize` is the public
  mechanism standard Chrome uses experimentally to give a correctly sized
  renderer surface time to arrive during bounds animation.
- Chromium's experimental asynchronous macOS live-resize path uses one
  in-flight compositor size and one replaceable latest size. It applies native
  geometry only when a matching compositor frame arrives.

These mechanisms establish two constraints for Limitless. A display refresh is
not evidence that the renderer can accept another resize, and a renderer visual
properties acknowledgement is not evidence that the matching surface has been
activated or presented. Limitless must add app-specific coordination without
creating a second owner for Chromium surface identity or fallback policy.

This ADR extends and replaces the viewport-resize details in
`2026-08-29-unify-chrome-state-and-animation-scheduling.md`. The earlier
decision to centralize chrome transitions and avoid frame-by-frame forced
redraws remains valid.

## Decision

Split resize ownership at the Chromium boundary:

- `WebContentsResizeCoordinator` is the single owner of desired browser-card
  geometry, Limitless transition lifecycle, and temporary presentation
  geometry. AppKit and SwiftUI report desired geometry to it, but do not resize
  or force layout of a Chromium subtree directly.
- The Objective-C++ Chromium bridge owns committed native viewport geometry,
  renderer backpressure, and commit and presentation acknowledgements for each
  visible `WebContents`.
- `BrowserCompositorMac` and `DelegatedFrameHost` remain the only owners of
  Chromium surface identifiers, fallback surfaces, activation, and deadline
  policy.

The coordinator will separate desired geometry, committed native geometry, and
presented Chromium geometry. Its app-level state machine has the following
conceptual states:

- `idle`: requested, committed, and presented sizes agree.
- `interactiveResize`: the native window is in live resize and target geometry
  may change continuously.
- `chromeTransition`: a finite Limitless chrome animation is changing the
  container while Chromium remains at stable committed geometry.
- `awaitingPresentation`: the final native size has been committed and the old
  presentation remains visible until Chromium reports successful presentation
  for the new viewport generation.

Every resize request will carry a monotonically increasing generation. Stale
commit or presentation callbacks must not release a newer resize operation.
The bridge will additionally track, per visible pane:

- At most one resize whose renderer commit acknowledgement is outstanding.
- At most one pending request, which is always replaced by the newest desired
  size and generation.
- Whether the latest request is terminal and therefore requires successful
  presentation before Limitless may remove temporary presentation geometry.

Renderer commit pacing and terminal presentation pacing are separate. A commit
acknowledgement permits the next pending resize to be sent. Only a successful
presentation acknowledgement for the terminal generation permits the visual
handoff.

### Interactive native window resize

`ContainerView` will use AppKit's live-resize lifecycle to enter and leave
interactive resize. Display-link scheduling may pace native chrome and
compositor-only presentation geometry, but it will not be the clock used to
send renderer viewport sizes.

Each desired size replaces the bridge's pending target. When no renderer resize
is in flight, the bridge commits that target. While a resize is in flight,
additional targets only replace the pending value. When Chromium acknowledges a
renderer commit for the expected pixel size, the bridge sends the newest pending
target, if any. A slow page may therefore skip viewport sizes without receiving
a backlog or blocking native chrome movement.

Until Chromium successfully presents the latest terminal generation, Limitless
will keep the hosted view's last complete presentation visible and use
coordinate-correct AppKit positioning, clipping, and bounded scaling to cover
the current browser card. The temporary geometry consists of paired
frame-to-bounds transforms: the viewport host scales the last presented card
into the desired container, while each visible `LimitlessBrowserView` applies
the inverse mapping from the desired logical viewport into its held frame. The
inner native `WebContents` frame and bounds advance to the desired logical size,
so Chromium can resize without exposing an incomplete surface. Chromium
continues to select the actual delegated fallback surface. Limitless does not
cache, copy, or advance a Chromium surface itself.

When live resize ends, the coordinator marks the newest desired target as
terminal and enters `awaitingPresentation`. If an earlier resize is still in
flight, the terminal target replaces the pending request and is dispatched when
that commit is acknowledged. Temporary presentation geometry is removed only
after the terminal target is successfully presented. If the terminal target is
identical to the in-flight or last committed size, the bridge upgrades that
request or observes the existing matching surface instead of waiting for a
no-op resize to produce another acknowledgement.

### Finite chrome geometry transitions

Sidebar, panel, sidebar-position, and similar animations will continue to hold
Chromium at stable committed geometry while SwiftUI animates the surrounding
chrome. A coordinate-correct compositor transform remains the normal
presentation path during the animation.

At logical animation completion, the coordinator will:

1. Commit only the latest settled viewport size to the active Chromium view.
2. Keep the old transformed presentation visible.
3. Wait for Chromium to acknowledge successful presentation for that viewport
   generation.
4. Remove the transform and reveal the correctly sized live surface in one Core
   Animation transaction.

The implementation must not clear the transform before the final surface is
ready. It must also handle an interrupted or immediately reversed transition by
invalidating the older generation and continuing from the currently visible
presentation without flashing an intermediate size. In split view, both panes
must successfully present the terminal generation before either transform is
removed.

### Chromium resize and surface contract

Replace `commitViewportLayout` with an asynchronous bridge contract that accepts
a desired size, generation, and terminal flag. Setting the wrapper and native
child frame and bounds will occur with implicit Core Animation actions disabled,
but it will not call `layoutSubtreeIfNeeded` from a SwiftUI update, AppKit
resize callback, or transition-completion callback. Advancing only
`RenderWidgetHostViewMac::SetSize` is insufficient for an embedded top-level
view because the Cocoa child is normally sized from the native `WebContents`
container. The bridge therefore advances that container's logical frame and
bounds as part of the same request.

The bounds change will flow through the existing `RenderWidgetHostViewMac`,
`BrowserCompositorMac`, and `DelegatedFrameHost` path. The bridge will enable
`SetShouldUseDefaultDeadlineOnResize(true)` while a coordinated resize is active
and restore the normal policy after successful terminal presentation,
cancellation, renderer failure, or teardown. This lets Chromium retain control
of its fallback surface and avoids a Limitless-specific surface cache.

The Chromium overlay will add a narrow public `content` API for resize
observation instead of including private `content/browser` implementation types
from Chrome code. It will expose two distinct signals:

1. A renderer commit acknowledgement containing the expected viewport pixel
   size. This releases bridge backpressure but does not release presentation
   geometry.
2. A successful presentation acknowledgement for an expected viewport pixel
   size and the `LocalSurfaceId` allocated for that resize. Internally, this
   signal may fire only after matching renderer metadata has arrived, the
   surface has activated in Viz, and the browser compositor reports successful
   presentation.

The bridge captures the Limitless generation in each callback, validates the
expected DIP size, device scale factor, pixel size, and surface, and rejects
callbacks for an obsolete generation or surface. This keeps the contract valid
when a window moves between displays during a resize. Swift does not receive or
compare Chromium surface identifiers. A generic
`RenderWidgetHostDidUpdateVisualProperties` callback is insufficient for the
terminal signal because it runs before the view embeds and presents the updated
surface.

Pending callbacks must resolve as cancelled when the `RenderWidgetHostView`,
renderer, tab, or window is destroyed. A two-second watchdog will emit
diagnostics and enter an explicit degraded state that keeps the last available
presentation clipped to the card. A timer must never be treated as proof that
the requested surface was presented.

### Presentation transform and input correctness

The current external `CALayer.sublayerTransform` changes pixels without changing
Chromium event or accessibility coordinates. It will not remain as an
unqualified rendering-only transform.

The selected implementation uses AppKit frame-to-bounds transforms because
they participate in native point conversion, accessibility geometry, and child
layout. During a hold, the hierarchy is coordinated as follows:

1. The viewport host has the desired container frame and the last presented
   container bounds. This scales the complete old browser-card presentation to
   follow native chrome.
2. Each visible `LimitlessBrowserView` retains its last presented frame and has
   bounds equal to its desired logical pane size. This is the inverse transform
   that keeps the inner `WebContents` in desired logical coordinates.
3. The inner native `WebContents` frame and bounds advance to the desired pane
   size, allowing its `RenderWidgetHostViewCocoa` child and renderer viewport to
   resize behind the held presentation.
4. `RenderWidgetHostViewCocoa` receives the desired local logical size
   explicitly while presentation mode is active. Its bounds feedback preserves
   that local size while normal AppKit conversion supplies the transformed
   window origin. This prevents an ancestor presentation scale from being fed
   back as another renderer resize.

The paired transforms cancel for coordinate conversion, so rendering, clipping,
hit testing, IME placement, drag coordinates, context menus, and accessibility
actions use the same desired logical viewport. After successful terminal
presentation, the coordinator makes every frame and bounds pair equal to the
desired geometry and disables presentation mode in one Core Animation
transaction.

If this coordinate-correct path becomes unavailable on a future Chromium or
macOS revision, the bridge must explicitly suppress pointer, gesture, drag, and
accessibility actions while a non-identity presentation transform is active.
Silently dispatching input using stale native geometry is not an acceptable
fallback. The suppression fallback cannot be the default-enabled
implementation.

Only visible `WebContents` participate in resize pacing. Hidden tabs retain
their previous size and synchronize when selected, preserving the existing
background-tab policy. Split view submits both visible panes as one generation
and uses a terminal presentation barrier so one pane cannot reveal its new
surface while the other still shows old geometry.

### View ownership and scheduling

`WebContainerView.ContainerView` will stop assigning browser subview frames
independently from both `layout()` and `setFrameSize()`. Those callbacks may
report new container bounds to the coordinator, but the coordinator alone will
decide whether to update presentation geometry or submit desired geometry to
the bridge. The bridge alone decides when renderer backpressure permits a native
viewport commit.

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

The test will record native desired sizes, bridge dispatches, renderer commit
acknowledgements, matching surface activation, successful presentation,
temporary transforms, page viewport changes, and frame timestamps. The
performance log format must include a Limitless resize generation and an opaque
Chromium surface identifier so events can be correlated without relying on
wall-clock ordering alone.

Performance criteria will run on reference 60 Hz and 120 Hz configurations when
the hardware supports them. The thresholds are:

- Native chrome presentation has no gap longer than three display intervals
  during either a 500 ms interactive window resize or a Command-S sidebar
  transition on the complex fixture.
- There is never more than one unacknowledged renderer resize per visible pane,
  the pending queue never contains more than one target, and an obsolete queued
  target is never dispatched after a newer target.
- A finite chrome transition performs one terminal viewport dispatch per
  visible pane. Split panes remove their temporary transforms in the same Core
  Animation transaction only after both terminal surfaces are presented.
- Across 30 runs on reference hardware, terminal successful-presentation
  latency from the final dispatch is at most 250 ms at the 95th percentile and
  no more than 500 ms in any run on the deterministic complex fixture.
- The final page viewport matches the settled browser-card size within one
  point. Temporary presentation geometry is removed within two display
  intervals of the matching successful-presentation signal.
- Consecutive captured frames contain no blank sliver, stale untransformed edge,
  or geometry discontinuity larger than one point at handoff.
- Pointer hit testing, selection, context menus, drag coordinates, IME
  placement, and accessibility bounds target the pixels displayed during a
  non-identity presentation transform. A deliberately lengthened test
  transition will exercise these paths.
- During a held transition, the renderer viewport reaches the desired terminal
  size even though the outer browser frame still represents the last presented
  geometry. The native `WebContents` frame and bounds, the
  `RenderWidgetHostViewCocoa` local bounds, and page `innerWidth` must agree.
- No synchronous `layoutSubtreeIfNeeded` call is reachable from the resize or
  transition path, and no Limitless-attributed browser UI task exceeds 50 ms on
  the deterministic fixtures.
- The reduced-motion path reaches the same final geometry without an animated
  hold and without synchronous subtree layout.
- Repeated Command-S toggles, transition reversal, window resize during a chrome
  transition, split view, multiple realized tabs, renderer crash, and window
  close do not leave the coordinator holding geometry.
- The two-second watchdog enters the logged degraded state without reporting a
  false presentation success or exposing an uninitialized surface.

The structural contract test will remain as a fast guard, but it is not evidence
that resize behavior is smooth.

## Rollout

1. Add generation-based resize instrumentation and the failing native-window
   plus Playwright end-to-end test.
2. Route all visible browser frame requests through
   `WebContentsResizeCoordinator`, remove duplicate direct frame assignment, and
   remove synchronous `commitViewportLayout` settlement.
3. Add the public `content` commit and successful-presentation observation API,
   with Chromium unit tests for size matching, activation, presentation,
   cancellation, and teardown.
4. Implement one-in-flight plus replaceable-latest bridge backpressure and add
   AppKit live-resize lifecycle handling. A display link may update native
   presentation geometry but must not dispatch Chromium viewport sizes.
5. Route resize fallback and deadline behavior through `BrowserCompositorMac`,
   `DelegatedFrameHost`, and `SetShouldUseDefaultDeadlineOnResize`.
6. Replace the external rendering-only transform with the coordinate-correct
   Chromium presentation transform and verify input and accessibility behavior.
7. Add terminal successful-presentation handoff for finite transitions and the
   split-pane barrier.
8. Validate the acceptance matrix on lightweight, complex, media, canvas, IME,
   accessibility, and split-view pages at 60 Hz and 120 Hz before enabling the
   new path by default and removing the old settlement path.

## Alternatives considered

### Continue resizing Chromium on every native layout callback

Rejected. It is simple, but a complex page makes renderer and compositor work
part of the perceived drag cadence. AppKit event coalescing cannot provide a
stable presentation contract.

### Pace Chromium viewport commits with a display link

Rejected. A display refresh says nothing about renderer capacity. On the pinned
macOS path, resize updates bypass an outstanding visual-properties
acknowledgement by default, so display-link pacing can still send work much
faster than a complex renderer can commit it. The display link remains useful
for native presentation, while Chromium viewport commits use renderer
backpressure.

### Enable Chromium's experimental resize flags globally

Rejected as the architecture. `kThrottleResizeIpc`,
`kUseDefaultDeadlineWhenAnimatingBounds`, and `kAsyncLiveResize` are useful
references and test configurations, but they are disabled by default and do not
provide the complete embedded-view contract Limitless needs. In particular,
they do not expose a terminal size-and-surface presentation acknowledgement to
the app coordinator. Limitless will reuse the established invariants and public
deadline mechanism without making correctness depend on an experimental global
flag.

### Treat renderer commit as successful presentation

Rejected. `RenderWidgetHostDidUpdateVisualProperties` runs before the matching
surface is embedded and presented. Releasing the temporary transform from that
signal recreates the blank-edge and stale-surface handoff race this decision is
intended to remove.

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
snapshot is not part of the selected architecture; the degraded path retains
Chromium's last available live presentation instead.

### Tune the SwiftUI animation or AppKit wake timer

Rejected. The reproduction shows that shortcut delivery occurs and that page
complexity changes the hitch by an order of magnitude. Timing changes do not
remove synchronous layout or establish a presented-surface handoff.

### Keep an external rendering-only Core Animation transform

Rejected. `CALayer.sublayerTransform` can move and scale pixels while AppKit,
Chromium input routing, IME, and accessibility continue to use untransformed
native geometry. A presentation transform must participate in coordinate
conversion or explicitly suppress affected actions while it is active.

## Consequences

- Native chrome and pointer tracking remain responsive even when a page cannot
  reflow at display cadence.
- Renderer resize throughput is bounded by renderer commits rather than display
  refresh rate. Complex pages may skip intermediate viewport sizes, but they do
  not accumulate resize work.
- Window resizing and Limitless chrome transitions share one geometry and
  presentation model instead of using unrelated policies.
- The final browser surface appears only when Chromium has actually presented
  the requested size, eliminating the forced-layout tail hitch and blank-edge
  races.
- Chromium remains responsible for `LocalSurfaceId` allocation, fallback
  surfaces, activation, and deadline policy. Limitless owns only desired
  geometry and its temporary presentation treatment.
- Resize behavior gains a bridge state machine and a narrow public `content`
  observation API. That API and its overlay tests must be maintained when the
  pinned Chromium revision changes.
- Coordinate-correct temporary transforms preserve pointer, IME, drag, context
  menu, and accessibility behavior. If the explicit suppression fallback is
  used temporarily, interaction is unavailable only while the transform is
  non-identity and the degraded path is observable in diagnostics.
- A slow page may temporarily show bounded compositor scaling while a newer
  surface is pending. This is preferable to blocking native interaction, and
  the handoff criteria limit how long that state can persist during normal
  operation.
- A stalled terminal presentation does not become successful because a timer
  elapsed. The watchdog retains the last available clipped presentation and
  reports a degraded state until presentation, renderer failure, cancellation,
  or teardown supplies a real terminal event.
