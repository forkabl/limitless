# Native workspace ownership and restoration

## Authority boundary

Chromium remains the only authority for pages. `TabListInterface`,
`TabInterface`, `TabHandle`, and the associated `WebContents` own tab lifetime,
physical order, active-page selection, navigation, discard and replacement,
window transfer, close, and session page restoration.

Limitless owns only product metadata:

- one stable product window UUID per eligible normal window;
- an ordered list of workspace UUIDs and user-visible names;
- one active workspace UUID and one last-active product tab UUID per workspace;
- one stable product tab UUID plus workspace membership for each live Chromium
  tab.

The controller stores `TabHandle` values, not `WebContents` or `TabInterface`
pointers. Every command re-resolves the handle and validates the exact live
window, profile, tab list, and product identifiers before using Chromium's tab
operations. SwiftUI receives one atomic workspace and visible-tab projection.
It never launches a page, owns a page, or keeps a second tab model.

## Window and profile scope

Every eligible normal window owns one `WorkspaceController`. Its default
workspace receives a new product-owned UUID and a CLI-validated name. Workspace
order is the controller's stable vector order. Visible tab order is always the
authoritative Chromium physical order filtered by active workspace membership.

Regular profiles keep independent metadata. A tab or product identifier from
another window is rejected, and a live tab from another profile receives a
distinct profile-mismatch result. Off-the-record windows receive fresh
ephemeral metadata, never serialize it through `SessionService`, and never
inherit regular-profile identifiers.

The launch policy is intentionally narrow:

- no `--limitless-workspaces` switch, or the value `enabled`, enables the
  feature;
- `disabled` and every unrecognized explicit value disable it;
- `--limitless-default-workspace-name` accepts trimmed, bounded UTF-8 and falls
  back to `Workspace` for empty, oversized, control, or bidirectional-control
  input.

These switches are CLI-visible policy seams. They are not a persistent or
authenticated settings plane.

## User operations

Creating a workspace appends its metadata, makes it active, and synchronously
opens a real Chromium new-tab page into it. Failure rolls the metadata back.
This prevents an empty workspace projection from covering a page owned by
another workspace.

Switching a workspace activates its last-active live Chromium tab, then its
first physical member, and creates a real new-tab page only when no live member
exists. Moving a tab changes only product membership. New tabs join the active
workspace. A Chromium activation of a hidden member makes that tab's workspace
active so page and rail state cannot diverge.

The existing Leader `n`, `p`, and `1` through `9` commands consume the filtered
active-workspace ordering. Shift+X close and `b` sidebar visibility retain
their reviewed behavior. No additional Leader keys are assigned by this
milestone.

## Live reconciliation

The controller observes the authoritative tab list and handles add, activation,
move, close, discard or replacement, transfer, close-all, tab-list destruction,
and window teardown.

- Reorder changes projection order without changing product tab IDs or page
  identity.
- Discard and replacement retain the live `TabHandle` assignment and attach the
  same product metadata to the current contents without retaining old
  `WebContents`.
- Transfer preserves the product tab UUID through temporary contents user data,
  removes the source membership, and joins the destination's active workspace.
  A source workspace UUID never crosses the window boundary.
- Close removes the dead handle and clears a last-active reference when needed.
- Close-all never creates a replacement page and teardown empties the published
  projection and invalidates product operations.

## Session persistence and deterministic recovery

Regular-profile metadata is stored in Chromium session window and tab
`extra_data`. The window envelope contains schema version, product window UUID,
active and default workspace UUIDs, ordered workspace records, names, and
optional last-active product tab UUIDs. Each tab envelope contains schema
version, product tab UUID, and workspace UUID. Envelope sizes, UTF-8, names,
UUIDs, duplicates, and schema versions are validated before acceptance.

The session service rebuild path must regenerate extra-data commands from the
live controller. Mutation-time commands alone are insufficient because a
session rebuild replaces its command stream. Restore begins a suppressed epoch
for each eligible normal window, accepts window metadata before tab metadata,
and publishes once after all restored tabs are reconciled. Startup tabs may
join the active workspace during this epoch, but only a tab with accepted
session metadata may supersede the saved last-active selection. A startup
placeholder cannot overwrite that selection before finalization.

`session_restore_in_progress` is an ephemeral controller epoch exposed only in
the live snapshot. It is never serialized. Typed workspace and split mutations
fail closed during the epoch, and split reconciliation must not dissolve a
Chromium-owned pair while only one member's product envelope has been applied.
Finalization closes the epoch after authoritative tab and native split
restoration, publishes the reconciled workspace projection, and then allows
split reconciliation to re-resolve exact live membership.

Profile-wide finalization snapshots controller `WeakPtr` values rather than
raw controller pointers. Any activation or real-tab creation route that can
run synchronous callbacks rechecks controller liveness before touching product
state. Runtime handles, weak references, and restore-epoch bits never enter the
window or tab envelope.

Recovery is deterministic:

- a missing or malformed window envelope creates fresh window, workspace, and
  tab identities;
- tab metadata is rejected when the window envelope was rejected;
- duplicate workspace UUIDs reject the window envelope;
- the first restored window or tab using a duplicated product UUID wins within
  the profile, and later duplicates receive fresh UUIDs;
- an unknown workspace UUID on an otherwise valid restored tab maps to the
  default workspace;
- a missing or malformed tab envelope under an accepted window envelope keeps
  the real Chromium tab, assigns a fresh product tab UUID, and maps it to the
  default workspace;
- a missing last-active tab falls back to the first physical member;
- an active workspace with no restored live member receives one real Chromium
  new-tab page;
- a transferred tab is serialized only in its destination window;
- a tab absent from Chromium's restored session is not resurrected by workspace
  metadata.

The clean-shutdown browser proof persists two windows and three workspace
records, including two in the source window, plus moved and transferred real
tabs, stable product identifiers, last-active pages, and a tab closed before
shutdown. A separate browser process resolves restored windows by product
window UUID and verifies exact metadata, page ownership, profile scope, and
absence of the closed page. A second PRE/post proof restores three native split
pairs across two workspaces while using only product UUIDs to resolve the new
process's live tabs. A hostile PRE/post proof rejects malformed tab metadata,
rekeys the later duplicate product tab UUID, and reconciles each affected
Chromium pair without serializing split state.

## Acceptance boundary

Deterministic browser tests prove the native SwiftUI projection, typed actions,
Leader ordering, multiwindow and profile isolation, private-window ephemerality,
corrupt-data recovery, close-all, teardown, clean restart, bounded workspace
selector layout, and in-process accessibility focus and action routing for the
workspace controls. The AX proof uses a same-process public AX query to
materialize SwiftUI's native tree, then traverses the real `NSHostingView`
controls from the rail root. Create and switch press the real buttons. Move
opens the exact row's native SwiftUI menu, focuses and presses a real menu item,
and verifies the authoritative tab membership changed. With 40
tabs and 12 workspaces, the closed tree stays bounded by O(tabs + workspaces +
agents), contains no destination items, and the opened menu contains exactly
11 destination items. No parallel or synthetic AX tree exists. The horizontal
Chromium tab strip is suppressed. Retained Chromium toolbar and other top
surfaces, plus the absence of an exact-window Retina PNG and external
VoiceOver, AX, IME, and hardware-input evidence, keep visual and external
interaction acceptance red.
