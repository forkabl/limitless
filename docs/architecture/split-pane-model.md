# Split pane ownership and interaction contract

Status: accepted implementation contract for the first Herdr-like split pane
milestone. `docs/features.md` items 10 and 11 remain the product authority.

## Authority and scope

The first milestone is a native two-pane browser split. Chromium remains the
only authority for tabs, pages, split membership, member order, active member,
layout, ratio, focus, prompts, fullscreen behavior, renderer crashes, and
session restoration. Limitless must not retain `WebContents`, persist a second
split model, serialize URLs or titles, or try to reconstruct a split after
Chromium declines to restore it.

The pinned Chromium substrate supports exactly two members and two layouts:
side by side and stacked. Recursive splits are outside this milestone.
`TabListInterface` supplies typed creation and unsplit operations. A
product-owned, per-window `SplitPaneController` may use the richer public
`TabStripModel` split APIs and observe `OnSplitTabChanged` for layout, ratio,
order, membership, and transfer changes that `TabListInterface` does not
expose. This direct dependency stays isolated in the adapter and needs no
product-specific mutation API. Three independent generic Chromium fixes remain
in the series: hostile-session ratio validation, the desktop Views
drag-completion observer described below, and macOS child-modal reactivation
through the existing topmost-dialog focus contract. They are exported as
separate patches with separate removal conditions and tests.

The adapter stores only its window/model lifetime references. Every snapshot
is rebuilt from live `TabInterface`, `SplitTabCollection`, and
`SplitTabVisualData` state. It never stores a tab, page, or raw split token as
durable product state. A snapshot identifies a pair with a transient key
derived from the sorted, unordered product tab UUIDs and separately reports
the authoritative ordered first and second members. SwiftUI and the future
shared command plane target the existing product window UUID and both product
tab UUIDs. Every pair mutation carries the caller's expected member order and
rejects a stale or reordered pair. Chromium split IDs are not authorization
tokens and do not cross the product boundary.

## Invariants and typed operations

Every operation re-resolves its target and fails closed unless all of these
conditions still hold:

- The product window and tab UUIDs are canonical and belong to the exact live
  normal window.
- All targets belong to that window's exact profile, are present in its active
  workspace, and refer to distinct live authoritative tabs.
- The browser is not closing, the tab list is editable, and no tab drag is in
  progress for the profile.
- Creation targets two unsplit tabs with matching pinned state and matching
  group membership, so Chromium never silently coerces either candidate's
  pin or group state. Pair operations target the exact current two-member
  Chromium split.
- Private windows, popups, stale targets, foreign windows, cross-profile
  targets, cross-workspace targets, and late calls after teardown are rejected.
  This explicit private-window rejection is part of the owner-provided split
  contract even though Chromium can maintain an ephemeral native private split.

The first typed surface contains create from two existing tabs, focus a pane,
set side-by-side or stacked layout, set a finite ratio, swap member order,
unsplit, and close the focused tab through the existing workspace controller.
Pair operations require the exact product window UUID plus both expected
ordered product tab UUIDs. Focus additionally requires the intended member.
Focus first activates that exact member through `TabListInterface`, then uses
Chromium's supported browser focus command to move the first responder into
the active page. Tests prove both active-member state and real page typing;
model activation alone is insufficient.
Every resolver and activation boundary is observer-capable. The controller
holds only WeakPtr lifetime authority across those callbacks, re-resolves the
exact product pair and active member after activation, and never invokes a
stale focus controller after its owner is destroyed. On macOS, Chromium's
generic topmost-dialog focus path must also reactivate a visible child dialog
after the native rail makes the browser window key.
The native UI exposes 25/75, 50/50, and 75/25 ratio choices. The adapter
accepts only finite ratios in the inclusive range 0.25 through 0.75 for this
milestone. Layout changes reset the ratio to 0.5, matching Chromium's existing
menu behavior.

Split eligibility inherits the existing native-shell and workspace policy:
the exact normal window must have the native rail and enabled product
workspaces. There is no second broad feature toggle. Typed validation reports
a distinct policy-disabled result when that prerequisite is false.

Existing Leader next, previous, and indexed-tab commands continue to operate
on active-workspace physical tab order, including both split members. Existing
close and sidebar commands keep their behavior. No split Leader keys are
assigned in this milestone. A later interaction review may choose bindings for
focus other pane, grow, shrink, orientation, swap, and unsplit. Until then,
`h`, `j`, `k`, `l`, `v`, minus, and `x` remain unclaimed.

A close result means Chromium accepted the close request. Before-unload or
tab-modal UI may defer or cancel it. Authoritative close/removal notifications,
not the synchronous return value, decide whether promotion completed. The
before-unload journey covers both cancel and accept. A separate real page
prompt journey proves exact split-pane host selection and macOS child-window
focus without treating app-modal before-unload UI as pane-modal evidence.

## Workspace and window reconciliation

A live split may never span product workspaces.

- Creating a split requires both members in the active workspace.
- Switching workspaces activates that workspace's last active real Chromium
  tab. If it is split, Chromium displays its same-workspace mate.
- Moving one member to another workspace first unsplits the pair, then moves
  only the requested tab. The survivor remains in the source workspace. If
  the safe unsplit cannot be completed, the move is rejected before metadata
  changes.
- A typed single-tab cross-window transfer follows the same unsplit-first
  policy. Chromium's own single-member drag/transfer may dissolve the split;
  observers reconcile the moved tab into the destination active workspace.
- Chromium may transfer an entire `SplitTabCollection` during a whole-pair
  drag. Limitless preserves it only when both authoritative members arrive in
  the same eligible destination window and active workspace. Otherwise it is
  dissolved. No product identifier or metadata authorizes a cross-profile
  mutation.
- External reorders, closes, discards, replacements, transfers, close-all, and
  teardown are reconciled from authoritative Chromium notifications. Discard
  or replacement keeps the tab identity. A renderer crash does not change
  split membership and need not emit a split notification: Chromium's SadTab
  remains page authority and the product projection stays unchanged. Closing
  one member leaves Chromium to promote the survivor.

Whole-pair detach and attach notifications are transactional. The adapter does
not repair an intermediate one-sided callback and never mutates the model from
inside `OnSplitTabChanged`. It posts one WeakPtr- and generation-guarded,
coalesced end-of-transaction reconciliation, then validates the final two
members, destination window, profile, and workspace after Chromium reports
insertion.
The eligible destination controller preserves the pair. An ineligible
destination controller dissolves it. A source controller never reaches into a
destination window.

Workspace and split observers feed one main-sequence combined projection. The
host coalesces notifications, rebuilds both workspace and native split state,
validates that every projected pair is same-workspace, and atomically replaces
the SwiftUI model. The rail never publishes a transient cross-workspace pair
between native unsplit and product membership updates.

### Desktop Views drag completion seam

`TabStripModel` does not otherwise publish a guaranteed notification after a
profile becomes editable at the end of a tab drag. Split reconciliation cannot
poll for that transition, and the ordinary insert, detach, and split callbacks
may all run while `TabListInterface::CanEditTabList()` is still false. A second
general Chromium patch therefore adds one observer notification for desktop
Views tab-strip drags. It is separate from the session ratio parser safety
patch.

After a successful `TabDragController::StartDrag()`, `TabDragController`
broadcasts the authoritative profile-wide `false` level to every currently
live exact-profile window with a `TabStripModel`. Its WeakPtr is captured before
`StartDrag()` and checked both immediately afterward and after the synchronous
observer broadcast, so neither a synchronous deletion nor a reentrant callback
can expose stale controller state. `BrowserTabStripController` stop callbacks,
the drag-controller destructor, and the final tab-bounds animation callback
schedule profile-WeakPtr-guarded recomputation. The animation callback runs only
after `ResetDraggingStateDelegate` has cleared every dragged view, with a
no-animation fallback and a replacement-drag guard. Recompute reads the
authoritative `TabListInterface::CanEditTabList()` value and broadcasts it to
every currently live exact-profile window with a `TabStripModel`. Duplicate
`false` levels are coalesced; the final successful transition is the sole
`true`. The product controller treats either callback level only as a wakeup,
revalidates authoritative editability and every target invariant, and only then
runs its coalesced reconciliation.

This is deliberately not a trigger contract for WebUI or other non-Views drag
implementations, arbitrary programmatic tab mutations, or every cause of tab-
list editability. A desktop Views-initiated drag still broadcasts its profile-
wide value to non-Views windows with an exact-profile `TabStripModel`. The
trigger covers desktop Views-initiated tab drags, which are the pinned desktop
Views drag substrate used by the adapter lifecycle proof. On supported
platforms, generic upstream interactive coverage performs a real drag and drop,
observes both existing same-profile source and destination tab models, requires
the first `false` while the drag controller is active, and requires one terminal
`true` only after the controller is inactive. Every delivered value must equal
the authoritative profile-wide editability at its callback. Chromium's
corresponding detach and direct-drop controls fail before destination active-
drag in the current macOS arm64 system-DnD harness, so the new generic route
follows Chromium's existing Mac arm64 disable. That native input gate remains
explicitly red. Deterministic product tests prove false propagation to the
SwiftUI projection and exact reconciliation policy without claiming a real Mac
arm64 drop.

Remove this seam when Chromium exposes a supported profile-scoped
post-drag editability notification that covers the required desktop transfer
routes after the final drag owner is destroyed, or when Limitless no longer
needs to reconcile externally dragged native splits. The ratio parser patch
has its own removal condition and must not absorb this lifecycle API.

## Native rail interaction

Each unsplit visible row has one sharp 22 point native SwiftUI menu button
labeled `Split <title> with another tab`. Its identifier is rooted in the
stable product tab UUID. Eligible destinations are built only while the menu
is open and contain at most the other visible tabs in the active workspace.
No pair candidates exist in the closed accessibility tree.

When the active workspace contains the active split, both rows expose their
first-pane or second-pane position and focused state without relying on color.
Each member's existing real tab-row `Button` is the focus action. Its label is
`Focus first pane, <title>` or `Focus second pane, <title>`, and its value is
`Focused pane` or `Visible pane`.
A constant-size compact split control strip near the Tabs header exposes:

- Side by side or stacked layout.
- 25/75, 50/50, or 75/25 ratio.
- Swap pane order.
- Unsplit while retaining both real tabs.

All surfaces use rectangles, zero corner radius, dense spacing, native
buttons/menus, native focusable semantics, stable accessibility identifiers,
and explicit labels. There is no synthetic accessibility tree. The closed rail
remains linear in tabs, workspaces, and agents. Opening one split destination
menu may temporarily materialize at most `tabs - 1` native menu items.

In a private or otherwise ineligible window, split controls are absent and the
projection truthfully reports split actions unavailable. Closed-tree tests cap
raw nodes at `48 + 8*T + 4*W + 4*A` and product identifiers at
`12 + 4*T + W + A`. No split destination identifier containing `-with-` may
exist while menus are closed. The overflow proof keeps 40 tabs, 12 workspaces,
and 12 agents present at the same time. In-process AX focus/action evidence is
required, while external keyboard and VoiceOver acceptance remain red.

## Restoration and hostile state

Limitless serializes no split data. Chromium's session commands restore an
exact two-member split, member order, layout, ratio, and the window-selected
tab. Background splits have no separately persisted active-member memory. The
existing versioned workspace envelopes restore only product window, workspace,
and product tab UUID metadata. The clean PRE/post browser proof resolves each
new process tab only by product tab UUID and workspace UUID. It never persists
or compares a prior-process `SplitTabId`; it asserts only that each ordered
restored pair shares one current live native split.

The proof restores three pairs across two workspaces: side by side at 50/50,
stacked at 25/75, and side by side at 75/25. It verifies authoritative member
order after a swap, Chromium's restored selected member, exact workspace
association, the native rail projection, live `MultiContentsView` geometry,
and real key routing into only the focused restored page. A bounded retained
startup placeholder, when Chromium supplies one, receives a fresh product UUID,
joins the saved active workspace, stays unsplit and non-active, and never enters
the persisted oracle.

During restore, the product's ephemeral workspace epoch disables mutations and
keeps native pairs out of the projection until all workspace envelopes are
authoritative. This prevents a temporary one-member workspace mismatch from
dissolving a valid native pair. Final workspace publication wakes split
reconciliation after the epoch closes.

Missing members, more or fewer than two restored members, or missing split
visual metadata follow Chromium's deterministic restoration behavior. A
missing member remains an ordinary tab. Missing visual metadata uses
Chromium's default side-by-side 0.5 state. Duplicate product tab UUIDs retain
the workspace controller's existing rule that every later authoritative tab
gets a new UUID. Duplicate or corrupt product window/workspace metadata must
never alias controllers; if reconciliation places native split mates in
different workspaces, Limitless dissolves the split rather than moving pages.

One or three or more restored members sharing a native token all remain
unsplit. Repeated native visual commands use the last valid command. A
malformed layout falls back to side by side. Empty or duplicated native split
tokens, including the same token in different windows, remain scoped to exact
live members and exact windows and never become product command identifiers.
An intermediate divider ratio is not persisted, so restart after a crash
during resize restores Chromium's last completed ratio.

Chromium's restored selected tab wins when it belongs to the restored active
product workspace. Workspace finalization refreshes that workspace's
last-active metadata from the Chromium selection instead of overriding it.
Only when the restored selected tab is absent or outside the restored active
workspace may product metadata select its validated saved member or the first
real member as a deterministic fallback.

Session split ratios are untrusted. Chromium's startup command parser must
reject non-finite values and values outside Chromium's general inclusive
`[0, 1]` domain before they reach layout. The stricter Limitless command range
does not change Chromium restoration authority: an externally created or
restored valid ratio such as 0.1 or 0.9 is projected truthfully even though it
does not match a product preset. The pinned parser lacks this validation, so
the implementation needs one narrow, general upstream safety patch with
explicit manifest symbols and hostile-state tests.

An invalid native ratio command is ignored before it mutates the session
record. A previous valid visual command remains authoritative; without one,
the split retains the safe default. Invalid visual metadata never discards a
tab or an otherwise valid two-member split.

## Evidence and acceptance limits

The primary outside-in journey must open the real lazy SwiftUI menu, invoke its
real destination item, and operate two distinct loaded pages without creating
an extra tab or `WebContents`. It preserves distinct page state and exact page
identities while verifying authoritative split identity, real row focus,
measured side-by-side and stacked bounds, ratio, order swap, unsplit, close
promotion, workspace switching, and unsplit-first movement. It also verifies
that the closed AX tree has no pair destinations and that the opened menu has
exactly the eligible active-workspace destinations. The baseline's focused
controller and native journeys cover exact same-window and workspace behavior,
stale-pair rejection, read-only edit projection, and the supported transfer
policy. Clean restart additionally covers exact pair order, both layouts,
25/50/75 ratios, selected-page input routing, workspace precedence, malformed
product metadata, and duplicate product tab UUID repair. The first
hostile-lifecycle checkpoint adds real before-unload cancel and accept
behavior, exact pane-hosted JavaScript prompt focus, renderer input after
dismissal, focus-controller destruction guards, and workspace-controller
destruction during split reconciliation. Follow-up bounded journeys must cover
the remaining hostile multiwindow, regular-profile, cross-profile, private,
popup, teardown, page-crash, and fullscreen cases without counting unrelated
workspace-only coverage as split acceptance.

The tests may inspect `MultiContentsView` and Chromium model state after the
native action, but must not use those internals to perform the user action.
Retries remain disabled, launcher jobs remain one, and every journey has an
external watchdog.

The cross-window transfer test is wired through Chromium's real desktop Views
horizontal tab strip so supported environments can exercise the pinned drag
controller and generic editability seam. The current Mac arm64 native system-
DnD route is an external red gate, so deterministic controller and SwiftUI tests
carry the local transfer and reconciliation evidence. None of this is a claim
that the native Limitless rail exposes a cross-window drag route. Native rail
split creation, focus, layout, ratio, swap, unsplit, close, workspace switch,
and workspace move remain covered through their real SwiftUI controls.

Upstream `MultiContentsView` currently retains rounded content corners,
insets, a rounded divider handle, and Chrome mini-toolbar surfaces. Those are
known visual blockers. The native rail can be behaviorally accepted while the
exact-window pixel, VoiceOver, hardware input, IME, and sharp split-surface
acceptance gates remain explicitly red until real evidence is captured.
