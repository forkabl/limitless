# Limitless v1 product roadmap

## Authority and intent

`docs/features.md` is the product source of truth. This roadmap orders the work
needed to deliver that vision without narrowing or reinterpreting it. The
architecture and handoff documents remain supporting engineering guidance. If
they conflict with `docs/features.md`, the product requirements win and the
supporting document must be reconciled.

V1 targets Apple silicon Macs. The product direction is a lean, sharp, fast
Arc-style browser with vertical tabs and workspaces, Herdr-like panes and leader
shortcuts, visible browser agents, agent-operable settings, fuzzy search, a
command palette, Chrome extensions, ad blocking, closed-tab undo, link Peek,
gradient themes, and optional AI adjacency that stays out of the way.

## Current baseline

Phase 1 provides a reproducible Limitless-branded Chromium build, a narrow
external resource seam, browser tests, and a real-app launch smoke test. The
WebUI candidate still uses Chromium's prototype `webui-browser-app`. A separate
BrowserView-hosted SwiftUI candidate now projects authoritative Chromium tabs
into a sharp native rail, groups them into product-owned workspaces, and shows
display-only agent presence from the current Herdr session. The native
candidate also implements the first per-window Leader shortcut map through one
product-owned controller. The native candidate now also projects and operates
Chromium-owned two-tab splits through compact, lazy SwiftUI controls without
persisting a parallel split model. Neither candidate is selected for
production, and neither has passed the exact-window pixel, external
accessibility, hardware-input, or IME gate.

| Product requirement | Current state | Next decisive proof |
| --- | --- | --- |
| 1. Vertical tabs | Authoritative projection, create and switch workspace, move, reorder, close, transfer, profile isolation, and clean-restart restoration pass in the native candidate; external native input, AX, and pixels remain open | Repeat the real workspace journey through an unlocked app with approved hardware input, VoiceOver, AX, IME, and Retina pixel evidence. |
| 2. tmux or Herdr shortcuts | First Leader milestone implemented: Ctrl+B arms n/p, 1-9, Shift+X, and b for tab and sidebar actions in each normal window; renderer, Views-control, and in-process AppKit routes are covered | Repeat the journey with external macOS hardware input, IME composition, VoiceOver, real extension conflicts, and the eventual shared command and settings planes. |
| 3. Chrome extensions | Partial Chromium substrate, unproven in Limitless | Install, permission, action popup, options, restart, restricted-page, incognito, disable, and uninstall journey. |
| 4. uBlock Origin or equivalent privileged blocking | Missing | A pinned, verified blocker handles a deterministic fixture, per-site exceptions, restart, update failure, and protected browser pages. |
| 5. Sharp compact UI | Dense zero-radius native rail exists; retained Chromium top surfaces and the missing exact-window PNG keep acceptance red | Approved pixel snapshots at multiple sizes and scale factors with clear focus, readable security state, and no excessive rounding. |
| 6. CLI-able and agent-updatable settings | A launch-only CLI policy can enable or disable Leader; this is not a persistent, authenticated, agent-updatable running settings plane | A typed, policy-aware setting changes a running profile through CLI and agent interfaces with validation, authorization, and auditability. |
| 7. Fuzzy tab and history search | Chromium search substrate only | One query returns separated live-tab and history matches, activates the right target, and never leaks another profile or incognito history. |
| 8. Command+Z reopens a closed tab | Missing; restore command exists | Non-editable browser chrome restores the exact tab while text fields and editable pages retain normal Undo. |
| 9. No Google password manager | Missing and contradicted by retained defaults | Fresh and migrated profiles expose no Google password-manager save, fill, generation, store, sync, or settings path while approved WebAuthn, HTTP auth, and third-party manager behavior still works. |
| 10. Herdr-easy split panes | Native baseline, clean restart, prompt anchoring, renderer recovery, split-window teardown, bounded same-profile transfer, and private/popup rejection are green; native fullscreen no longer crashes, while completion still stalls at the same waiter as stock Chromium | Prove external fullscreen, Mac arm64 system drag, hardware input, VoiceOver, AX, IME, and exact-window pixels. |
| 11. Herdr for browsers | Display-only presence, the first Leader command map, real browser workspaces, and the first two-pane browser journey exist; shared command settings and agent authorization remain missing | Complete the split lifecycle gates, then expose reviewed split commands through the shared typed command and settings planes. |
| 12. AI-compatible, not intrusive | Current-session Herdr presence is visible without controls; authentication, browser authorization, profile scope, revocation, and audit remain missing | Authenticated agents are optional, visible only when present, explicitly scoped, revocable, isolated by profile, and fully audited. |
| 13. Link Peek | Missing | A transient browser-owned page shows trustworthy origin, handles hostile behaviors, promotes without reload, and dismisses without session pollution. |
| 14. Spotlight command palette | Missing; command substrate exists | Keyboard-only palette searches commands, tabs, and history, reflects live enablement, rejects stale targets, and restores focus on cancel or completion. |
| 15. Gradient themes | Missing | Theme changes through UI and CLI, persists, follows light and dark appearance, and falls back safely for high contrast and reduced transparency. |

## Ordered end-to-end delivery

### 1. Gate 2A: prove the Chromium integration seam

Complete one thin real-browser journey before broad product shell work:

1. Launch the real `.app` with a temporary profile and deterministic local page.
2. Enter a destination through browser chrome and navigate the real active tab.
3. Verify title, URL, favicon, loading, security state, and rendered page content
   are projections of Chromium state.
4. Exercise back, forward, reload, and stop through live command enablement.
5. Open, switch, and close a second real tab without recreating either page.
6. Relaunch and verify session restoration.
7. Open DevTools, an extension action popup, and representative security or
   permission surfaces through retained Chromium UI.
8. Prove forward and reverse focus traversal, dead keys, composed text, one CJK
   IME, and keyboard-only operation across shell and page.
9. Inspect one coherent accessibility tree with VoiceOver and Accessibility
   Inspector.
10. Exercise native dragging, resizing, traffic lights, minimize, zoom, browser
    and tab fullscreen, multiple displays, and close cancellation.
11. Crash and recover the shell renderer without losing browser-owned tabs.
12. Prove that stale, cross-window, and cross-profile tab handles are rejected.

Gate 2A stops if SurfaceEmbed produces unstable rendering, split accessibility
trees, broken focus or IME, hidden security UI, or a broad Chromium patch.

### 2. Gate 2B: honor the native SwiftUI requirement

The current WebUI architecture does not satisfy the literal SwiftUI product
requirement. Before broad shell implementation, compare the Gate 2A journey with
a bounded AppKit/SwiftUI host inside Chromium's existing `BrowserView` window
composition. The comparison must not create a third `BrowserWindow`, directly
reparent tab native views, or duplicate Chromium session and tab state.

Use identical navigation, tab, prompt, extension, focus, IME, accessibility,
window, fullscreen, crash, startup, input-latency, and pixel-quality checks for
both candidates. Select the durable seam from evidence, then reconcile the
architecture decision documents. Development cost is not a deciding factor.

### 3. Establish shared product control planes

Build three narrow foundations before feature-specific settings proliferate:

- a typed, versioned, policy-aware preference schema with explicit profile
  selection, effective-value reporting, authorization, and an audit trail;
- a single command registry backed by Chromium command enablement and used by
  menus, shortcuts, the palette, settings, and agents;
- a workspace and agent-session model that references authoritative Chromium
  identities and reconciles session changes without serializing duplicate tabs.

The first CLI journey changes one harmless appearance preference in a running
profile, reads it back through the same schema, and rejects invalid, managed,
cross-profile, and unauthenticated writes.

### 4. Deliver the sharp vertical workspace shell

Build the first product-owned composition around real Chromium state:

- a compact vertical tab and workspace sidebar;
- a distinct agents section that is absent or quiet when no agents are active;
- destination entry and essential navigation controls;
- native drag exclusions, traffic-light spacing, minimum-size behavior, and
  clear security and capture state;
- semantic design tokens, sharp geometry, restrained motion, and explicit
  startup plus input-to-paint budgets.

The E2E proof uses three tabs across two workspaces, moves and restores them,
checks page state, uses keyboard and VoiceOver only, and compares approved Retina
snapshots at normal and minimum window sizes.

### 5. Add command-first operation

The first discoverable Leader state machine is now implemented as a bounded
per-window milestone. Its map is intentionally limited to tab navigation,
indexed tab selection, close, and sidebar visibility. Next, route it through the
shared live command and settings planes, add Command+Z closed-tab undo without
stealing text Undo, and deliver the Spotlight-style command palette. Text
editing, page and extension shortcuts, IME composition, VoiceOver, and
macOS-reserved shortcuts always take precedence where appropriate.

The palette then adds fuzzy open-tab and history search using Chromium-owned
providers and explicit profile boundaries. No parallel history index is
permitted.

### 6. Add Herdr-like split browsing

Start with Chromium's supported two-pane split in the native BrowserView
candidate. The first baseline now proves creation, real-page focus, resize,
layout, swap, unsplit, close promotion, workspace switching, unsplit-first
workspace movement, strict target validation, and a lazy linear native control
surface. Clean PRE/post restart now resolves new live tabs only by product UUID
and proves exact pair order, both layouts, 25/50/75 ratios, selected-page input,
workspace precedence, malformed metadata recovery, and duplicate UUID repair.
Prompt anchoring and controlled renderer crash/reload recovery are now green.
The tabless shell's native fullscreen entry no longer crashes, but completion
remains an external red gate because the identical stock Chromium control
stalls in this environment. Real split-window teardown now cancels pending rail
projection without stale callbacks. A bounded same-regular-profile transfer
proof preserves an eligible whole Chromium pair and dissolves a transferred
single member through Chromium's typed APIs, with no product transfer state.
Private product split commands and popup rail construction fail closed while
leaving Chromium-owned page and split authority untouched. Next prove the
remaining external fullscreen, Mac arm64 system-drag, hardware-input,
VoiceOver, AX, IME, and exact-window pixel gates. Then extend to recursive
Herdr layouts if required to fulfill the product behavior, preserving Chromium
ownership of every underlying tab.

### 7. Preserve extensions and remove Google password manager

Complete the Chrome extension journey before changing the toolbar or shipping a
blocker. Preserve permission, popup, options, DevTools, restricted-page,
incognito, policy, and extension-command behavior.

Separately remove Google password-manager save, fill, generation, store, sync,
and settings surfaces from fresh and migrated profiles. Keep HTTP auth, WebAuthn,
and third-party password-manager extensions as separately tested capabilities,
not accidental casualties of the removal.

### 8. Add Peek, visible agents, and quiet AI adjacency

Implement link Peek with a transient browser-owned `WebContents`, trustworthy
origin presentation, strict permission and popup handling, promotion without
reload, and clean dismissal. Add an authenticated browser-process agent registry
with visible identity, current scope, explicit grants, revoke or kill controls,
profile and incognito isolation, and a durable audit trail. AI actions remain
optional and contextual. A browser with no connected agents shows no ambient AI
chrome.

### 9. Add themes and ad blocking

Add gradient themes through the typed preference and design-token systems, with
safe high-contrast, reduced-transparency, light, and dark variants. Enforce
contrast, focus, text scaling, RTL, and reduced-motion behavior in screenshots
and accessibility tests.

Evaluate a signed MV3 uBlock-compatible path first, including uBlock Origin Lite
or an equivalent product DNR extension. Measure it against deterministic
blocking, cosmetic filtering, update integrity, performance, privacy, and site
breakage acceptance criteria. Escalate to a Brave-inspired engine only when
measured product gaps justify the larger security and maintenance surface. Never
weaken extension security policy or suppress Chromium interstitials.

### 10. Harden and ship the Apple silicon Mac app

Complete signing, notarization, updates, crash reporting, migration, supported
macOS-version coverage, mixed-display behavior, performance budgets, privacy
review, and release-channel identity. V1 does not claim Intel, Linux, or Windows
support.

## Current milestone and next product slice

The bounded native workspace milestone is implemented in the
BrowserView-hosted SwiftUI candidate. Each eligible normal window receives a
stable default workspace plus ordered product-owned workspace, window, and tab
identities. These records reference live Chromium `TabHandle` values and never
own or retain `WebContents`. Creating a workspace opens one coherent real
Chromium page. Switching filters the rail and restores the last active live tab,
moving changes only product membership, and physical order continues to come
from Chromium.

New tabs join the active workspace. Close, reorder, discard or replacement,
window transfer, close-all, and teardown reconcile without stale handles or
cross-window mutation. Regular profiles are isolated, off-the-record metadata
is fresh and ephemeral, and typed product operations reject stale, foreign
window, and foreign profile targets. Window and tab envelopes persist through
Chromium session extra data. A clean PRE/post browser restart proves stable
identities, order, active workspace and pages, destination-only transfer, and
non-restoration of a page closed before shutdown.

Ctrl+B still arms the existing controller owned by one normal browser window.
While armed, n and p select the next or previous tab in the active workspace,
1 through 9 select its exact one-based visible index, Shift+X closes the active
tab, b toggles the native sidebar, and Escape or a two-second timeout cancels
the mode. Keys for unshipped pane commands remain unclaimed.

Renderer-originated events, native Views controls, and in-process macOS
APPLICATION events use the same controller and timeout semantics. Coverage
exercises page typing, Cmd+L fallthrough after disarming, identical-native-event
deduplication, exact-window isolation, popup exclusion, disabled fallback, and
armed-window teardown. The APPLICATION journey is in-process AppKit routing
evidence, not external hardware-input acceptance. A separate in-process AX
journey traverses and invokes the real SwiftUI create, switch, and move-menu
controls. Its 40-tab, 12-workspace overflow case proves the closed native AX
tree is linear and destination items exist only while one move menu is open.
External hardware input, IME, VoiceOver and AX behavior, and exact-window pixel
review remain red.

The launch policies are deliberately narrow:
`--limitless-leader-shortcuts` being absent or explicitly set to `enabled`
enables Leader, while `disabled` and every other explicit value disable it.
Workspaces follow the same enabled, disabled, and invalid fail-closed policy via
`--limitless-workspaces`; `--limitless-default-workspace-name` supplies a
validated default name. These are CLI-visible launch policies, not persistent
preferences, live settings for a running profile, or an authenticated and
audited agent settings plane.

The native rail still shows every agent returned by the current Herdr session
without fabricating rows or granting browser capabilities. It does not aggregate
named sessions, identify every agent using Limitless, or authenticate and
authorize agent commands. Those gaps remain separate from the Leader milestone.

The first real Herdr-like split-pane baseline is now implemented against
Chromium's authoritative two-tab split substrate. Product commands use stable
workspace tab UUIDs, re-resolve exact live window and profile authority, and do
not persist URLs, titles, `WebContents`, raw handles, or parallel split state.
The sharp native rail opens one lazy O(T) eligibility menu, projects existing
pairs even while mutation is unavailable, and keeps its closed accessibility
surface linear. No h, j, k, l, v, minus, or x binding is assigned before an
interaction review.

The clean split PRE/post journey restores three Chromium-owned pairs across two
workspaces at 25/50/75 ratios and both layouts, including swapped authoritative
order and the selected active member. The post process resolves pages only by
stable product UUIDs, checks the live rail and `MultiContentsView`, and routes a
real renderer key only to the focused restored page. A hostile companion
journey rejects malformed tab metadata, rekeys the later duplicate product tab
UUID, dissolves only the now cross-workspace pair, and preserves the otherwise
valid native pair. Limitless persists no split identifier, page, ratio, order,
or layout state.

The bounded hostile-window checkpoint now covers split-window teardown,
eligible whole-pair and single-member same-profile transfer semantics, and
private/popup rejection without parallel product split or transfer state.
Native fullscreen completion remains an external red gate alongside the Mac
arm64 system-DnD harness, which cannot reach destination active-drag even in
established Chromium controls. Those external gates remain red while
deterministic lifecycle and projection coverage stays green. The shared command
and persistent settings planes remain the next cross-cutting prerequisite
before broad palette or agent control work. `docs/features.md`
remains untouched and authoritative.

## Delivery discipline

- Start with the real user journey and add the narrowest browser-process or unit
  coverage needed to diagnose it.
- Keep Chromium patches explicit, reviewable, and smaller than product-owned
  code. Update the manifest whenever the patch surface changes.
- Never expose arbitrary preferences, commands, page scripting, profile files,
  or cross-window handles to the shell, CLI, extensions, or agents.
- Treat focus, IME, accessibility, security identity, native window behavior,
  and retained Chromium surfaces as acceptance criteria, not polish.
- Inspect the built UI at Retina resolution and multiple window sizes. Fix
  clipping, weak focus, excessive rounding, visual drift, and flaky tests before
  moving on.
- Commit and push each coherent, validated checkpoint without including
  user-owned changes to `docs/features.md`.
