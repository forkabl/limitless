# Limitless Browser: Codex Project Handoff

## Purpose

This file is supporting architecture and engineering guidance for continuing
development of Limitless with Codex. `docs/features.md` is the ultimate product
source of truth and must not be narrowed, replaced, reinterpreted, or edited
unless Arham explicitly requests it. `docs/v1-product-roadmap.md` orders the
current work. If this handoff conflicts with either document, reconcile the
handoff rather than changing the requested product vision.

The original Phase 0 architecture assignment in this file is retained as
history. Current execution starts from the checked-in roadmap, architecture
records, patch manifest, and the active repository state.

## Mission

Build a production-quality Apple silicon Mac browser for v1 that:

- uses Chromium as the browser engine and retains its mature browser machinery;
- presents a fully opinionated Limitless-owned user interface;
- remains practical to rebase onto newer Chromium revisions;
- minimizes direct changes to upstream Chromium source;
- preserves platform-appropriate window behavior, accessibility, input, and system integration;
- is tested in the same way an end user experiences it.

Development cost is not the primary decision criterion. Prefer quality, simplicity, robustness, scalability, and long-term maintainability.

Intel Macs, Linux, Windows, Android, and ChromeOS are outside the v1 build,
test, packaging, and release scope. Retain useful portability evidence and avoid
unnecessary CPU-specific design, but do not let deferred platform parity block
or redefine the Apple silicon Mac product.

## Current continuation state

The current bounded product-facing candidate is a BrowserView-hosted SwiftUI
workspace and tab rail. It projects authoritative Chromium tab state, suppresses
upstream tab strips for that window, and includes a display-only Agents section
backed by the current Herdr session. The first Leader milestone is now
implemented through one controller per eligible normal window. Ctrl+B arms n/p
for previous and next tab, 1 through 9 for exact indexed tabs, Shift+X for close,
and b for sidebar visibility; Escape and a two-second timeout cancel the mode.

The same controller serves renderer-originated events, native Views controls,
and in-process macOS APPLICATION routing. Coverage includes ordinary page
typing, Cmd+L fallthrough, identical-event deduplication, exact-window
isolation, popup exclusion, disabled fallback, and armed-window teardown. The
CLI launch policy enables Leader when
`--limitless-leader-shortcuts` is absent or set to `enabled`; `disabled` and
invalid explicit values disable it. This is not a persistent preference, a live
running-profile settings API, or an authenticated and audited agent-control
plane.

The presence section still does not authenticate or authorize agents, grant
browser capabilities, aggregate named Herdr sessions, or prove that every
visible agent is using Limitless. In-process AppKit routing does not satisfy the
external hardware-input gate. Exact-window pixels, external accessibility and
VoiceOver, IME, hardware input, and retained Chromium top-surface acceptance
remain red. See `docs/v1-product-roadmap.md` and
`docs/architecture/gate-2b-swiftui-boundary-evidence.md` for the current gate.

The native rail now includes real workspaces and Chromium-owned two-tab splits.
A clean PRE/post process restart restores three pairs across two workspaces with
exact product UUID membership, authoritative order, both layouts, 25/50/75
ratios, selected-page input routing, and hostile product-metadata recovery.
Limitless persists no split model. The next product-facing priority from
`docs/features.md` is the remaining hostile split lifecycle. Prompt anchoring
and controlled renderer crash/reload recovery are green. The tabless shell no
longer crashes on native fullscreen entry, while completion remains externally
red at the same waiter as stock Chromium. Split-window teardown, bounded typed
same-regular-profile transfer, single-member authoritative dissolution, and
private/popup product boundaries are now green without parallel transfer state.
The next product milestone is the shared control plane for features 2, 6, 11,
and 14. Its foundation defines typed versioned preference requests and a
stateless command registry; it does not yet claim a running-profile adapter,
live SwiftUI preference projection, or external CLI transport. Continue with
the registered Chromium preference, exact-profile authorization, restart
persistence, audit transport, and live routing of Leader and split commands.
Do not replace this ordering with the historical phase text below when
continuing work.

## Fixed project context

The workspace root contains two sibling directories:

```text
limitless-workspace/
├── limitless/
└── chromium/
    ├── .gclient
    └── src/
```

Repositories have different responsibilities:

- `limitless/` is the product repository. Put Limitless-owned source, design documentation, scripts, tests, patch metadata, and integration tooling here whenever technically reasonable.
- `chromium/src/` is the upstream Chromium checkout. Treat it as a large third-party dependency, not as the natural home for product logic.

The Chromium baseline is pinned to this exact commit:

```text
2b4613f0a24d234b139f1246028239a2f5dde58c
```

The full SHA should be recorded in:

```text
limitless/chromium.version
```

The checkout may have been created without full history. A short `git describe` result is therefore not evidence of a problem.

## Product boundary

Limitless should own the visible browser experience. Chromium should continue to supply the browser systems that are expensive, security-sensitive, or standards-sensitive, including as many of these as the chosen architecture permits:

- Blink rendering and site isolation;
- Chromium's multi-process model and sandboxing;
- navigation, history, profiles, preferences, sessions, and tab lifecycle;
- network stack, cookies, cache, proxy, certificates, and authentication;
- downloads, permissions, content settings, find, printing, and page zoom;
- extensions where product requirements allow them;
- DevTools;
- accessibility foundations;
- crash handling and recovery;
- platform integration and native browser-window behavior;
- Chromium security fixes and release updates.

Do not accidentally reimplement browser machinery merely to obtain a different visual shell. At the same time, do not let an easy integration route force Limitless into Chromium's existing visual or interaction model.

The exact Limitless information architecture and visual layout are separate product decisions. Do not infer a conventional tab strip, toolbar, or horizontal chrome if no product specification requires one.

## Required operating rules

1. Read all applicable `AGENTS.md` files before doing work in a directory. More specific instructions override broader ones.
2. Never edit generated files by hand. Never manually edit `CHANGELOG.md`.
3. Never add the agent name as a commit co-author.
4. Preserve unrelated user work and existing local changes.
5. Before fixing a bug, reproduce it end to end as closely as possible to the way a user encounters it.
6. Treat visible quality as a release concern. During E2E work, inspect the rendered UI carefully and fix clear layout, spacing, clipping, focus, animation, and platform-integration defects found along the way.
7. Treat engineering quality the same way. Do not ignore lint failures, test failures, or flaky tests because they appear unrelated. Investigate and fix them, or stop and document why doing so would be unsafe or out of scope.
8. Prefer the simplest stable architecture, not the fastest prototype.
9. Make claims from the checked-out Chromium source at the pinned commit. Do not rely solely on memory, current online documentation, or paths from a different Chromium revision.
10. Do not make broad mechanical changes to Chromium to hide existing UI. Find ownership boundaries and lifecycle seams first.
11. Keep platform-specific code behind explicit interfaces. Keep product behavior and UI shared where that does not damage native behavior.
12. Keep commits small, coherent, and easy to rebase or drop. Do not mix formatting churn with functional changes.
13. Do not silently weaken Chromium security features, sandboxing, site isolation, permission checks, safe browsing behavior, or process boundaries to make UI integration easier.

## Definition of a minimal upstream patch surface

The preferred order of solutions is:

1. Limitless-owned code linked into the Chromium build through explicit build targets and narrow registration or factory seams.
2. Existing Chromium extension points, delegates, observers, controllers, model APIs, and feature flags.
3. Small adapter or hook patches that redirect construction or ownership to Limitless code.
4. A small, maintained patch series against Chromium when no stable hook exists.
5. Direct modification of Chromium feature implementations only as a last resort.

Minimal means more than a low line count. A good patch surface:

- touches few upstream ownership boundaries;
- avoids copied Chromium implementations;
- has narrow, stable intent;
- contains no product styling or business logic;
- is independently testable;
- fails clearly when an upstream rebase invalidates it;
- can be explained in a short patch manifest;
- does not fork security-critical behavior.

When a direct Chromium patch is unavoidable, record at least:

- the upstream files and symbols touched;
- why no existing seam was sufficient;
- the Limitless code that depends on it;
- how it is tested;
- expected rebase conflict signals;
- a removal or upstreaming path, if one exists.

## Initial workspace verification

Before architecture work, verify and report the following without changing the pinned revision:

```bash
cd <workspace>/chromium/src
git rev-parse HEAD

cd <workspace>/limitless
cat chromium.version
```

Both values must be:

```text
2b4613f0a24d234b139f1246028239a2f5dde58c
```

Also inspect:

- repository status in both repositories;
- applicable `AGENTS.md` files;
- current Limitless source and build layout;
- existing build scripts, patch tooling, tests, and design documents;
- the actual Chromium build configuration already used on this machine.

Do not reset, discard, clean, or rewrite existing work. If the Chromium checkout does not match the pin, stop and report the mismatch instead of moving it automatically.

## Phase 0: Architecture research

### Goal

Produce an evidence-backed architecture recommendation for replacing or bypassing Chromium's standard visible browser chrome while retaining Chromium browser behavior.

Do not implement the product shell during this phase. Small disposable experiments are allowed only when source inspection cannot answer a specific question. Label and isolate them so they cannot be mistaken for production architecture.

### Research method

Trace behavior from actual construction and event entry points instead of searching only for class names. For each important path, identify:

- object ownership and lifetime;
- UI toolkit and rendering boundary;
- model or service underneath the UI;
- commands, delegates, observers, and controllers involved;
- platform-specific branches;
- test coverage and available test harnesses;
- likely stability across Chromium rebases.

Record exact file paths, symbols, and a brief explanation of why each item matters. Line numbers are helpful during research but should not be treated as stable architecture identifiers.

### A. Browser window and top-level UI ownership

Map how a normal desktop browser window is created and destroyed from browser startup through the native window and the content view. Identify, at minimum:

- browser creation and startup entry points;
- the `Browser` or equivalent browser-window model ownership;
- `BrowserWindow` interface and implementations;
- `BrowserView`, frame, non-client frame, root view, and layout ownership where present;
- creation and ownership of tab UI, toolbar, location bar, status UI, side panels, bookmarks UI, downloads UI, permission prompts, and dialogs;
- native title bar, caption buttons, draggable regions, window controls, resizing, fullscreen, and picture-in-picture interactions;
- places where platform implementations differ on macOS, Linux, and Windows.

Answer: what is the narrowest seam at which Limitless can own the entire visible browser shell without forking browser behavior?

### B. Browser models and commands

Map the stable non-visual APIs the shell can call or observe:

- tab or web-contents model and selection changes;
- navigation controller and navigation state;
- command routing, command enablement, keyboard accelerators, menus, and context menus;
- address input, search, autocomplete, and omnibox services;
- profiles, preferences, history, bookmarks, sessions, recently closed items, and sync boundaries;
- downloads and download lifecycle;
- permissions, page actions, content settings, identity, security state, and certificate UI;
- find, zoom, print, save, share, translate, and media state;
- extensions and extension actions;
- DevTools attachment and opening behavior.

For each capability, distinguish model or service code from Chromium's current Views or WebUI presentation.

### C. Candidate UI architectures

Investigate at least these candidates, plus any better seam revealed by the source:

1. A Limitless-owned Chromium Views shell that consumes existing browser models and services.
2. A WebUI or web-technology shell hosted inside native Chromium window infrastructure.
3. A hybrid shell with native windowing and security-sensitive surfaces plus a web-rendered product UI.
4. Replacement of the normal browser window implementation through an existing or narrowly added factory or delegate seam.

Compare candidates using evidence from the pinned source. Use these criteria:

- completeness of visual and interaction control;
- patch surface and expected rebase burden;
- access to browser models and privileged commands;
- startup and runtime performance;
- memory and process implications;
- accessibility;
- keyboard focus, accelerators, drag-and-drop, and IME behavior;
- native title bar and window controls;
- menus, prompts, popups, and secondary browser windows;
- testability;
- macOS, Linux, and Windows parity;
- security boundary complexity;
- feasibility of incremental delivery.

Do not choose based on familiarity alone. Explicitly identify irreversible or expensive architectural commitments.

### D. Cross-platform behavior inventory

Create a platform matrix for macOS, Linux, and Windows that covers:

- native frame versus custom-drawn frame;
- caption buttons and traffic-light controls;
- window dragging and resizing;
- system menus and global or application menus;
- keyboard shortcuts and modifier conventions;
- fullscreen and native fullscreen;
- multiple windows and profiles;
- high-DPI and fractional scaling;
- light, dark, high-contrast, and forced-colors modes;
- accessibility APIs, screen readers, focus indication, reduced motion, and keyboard-only operation;
- input methods, composed text, RTL, and localization expansion;
- notifications, file pickers, authentication dialogs, certificates, permissions, and system integration;
- GPU and software-rendering fallbacks.

The shared design may be opinionated, but it must not pretend all three platforms have identical window semantics.

### E. Chromium-owned UI that cannot simply disappear

Inventory user-visible surfaces that may be created outside the main toolbar or tab strip. Examples include:

- permission bubbles and prompts;
- HTTP authentication and certificate surfaces;
- downloads, print preview, find, save-page, and file chooser flows;
- DevTools and view-source windows;
- extension popups and options;
- profile picker, first-run, crash restore, and session restore;
- task manager and settings/history pages;
- translate, password, autofill, payment, and identity surfaces;
- media, casting, picture-in-picture, and sharing indicators;
- error pages, interstitials, captive portal, and safe browsing warnings;
- popups, app windows, installed web apps, and secondary window types.

Classify each surface as:

- retained unchanged initially;
- restyled through an existing seam;
- wrapped by Limitless UI;
- replaced later;
- intentionally unsupported, with product approval required.

### F. Build and repository integration

Research how Limitless-owned code should enter Chromium's GN/Ninja build without spreading product files through the upstream tree. Determine:

- where GN targets can live;
- what minimal upstream dependency or registration change is needed;
- how branding, resources, strings, icons, feature flags, and generated resources are handled;
- how platform-specific sources are selected;
- how a developer builds and launches the Limitless binary;
- how CI reproduces the pinned checkout and applies any patch series;
- how rebase conflicts and unexpected upstream drift are detected;
- whether an existing Chromium branding/product mechanism is useful without constraining the architecture.

Prefer deterministic scripts over undocumented manual edits.

### Required Phase 0 deliverables

Write these documents under `limitless/docs/architecture/` unless the repository already has a clearly better documentation convention:

```text
docs/architecture/
├── chromium-window-map.md
├── browser-capability-map.md
├── ui-architecture-options.md
├── cross-platform-matrix.md
├── chromium-ui-surface-inventory.md
├── build-integration.md
├── patch-surface.md
└── decision-record.md
```

`decision-record.md` must contain:

- the recommended architecture;
- alternatives considered;
- source evidence at the pinned revision;
- ownership and lifecycle diagrams;
- the proposed Limitless-to-Chromium interface boundary;
- expected upstream patches;
- security and accessibility consequences;
- platform-specific consequences;
- known unknowns and validation experiments;
- migration and rebase strategy;
- a thin vertical slice proposal.

Use Mermaid diagrams only if the repository's Markdown renderer supports them. Otherwise use concise text diagrams.

## STOP/RETHINK GATE 1: Architecture approval

Stop after Phase 0. Do not begin the production shell until the user reviews the research and explicitly approves an architecture.

Recommend rethinking the architecture if any of these are true:

- the proposed route requires copying large Chromium UI implementations;
- product logic must be spread across many upstream directories;
- the shell would bypass security-sensitive browser services;
- core browser behavior would need to be rebuilt outside Chromium;
- cross-platform window behavior cannot be expressed behind a clean boundary;
- basic accessibility or keyboard behavior is unclear;
- the expected patch series is broad, fragile, or hard to test;
- a rebase would require manual rediscovery of the same integration points;
- the architecture only works for the main happy-path browser window.

At this gate, present the recommendation, the estimated patch surface, the unresolved risks, and the smallest experiment that would resolve each risk.

## Phase 1: Reproducible foundation

After architecture approval:

1. Establish deterministic developer scripts for verifying the Chromium pin, applying or checking patches, generating build files, building, and launching.
2. Define a Limitless-owned GN target structure with shared and platform-specific layers.
3. Add a small patch manifest if upstream changes are required.
4. Add a build-time Limitless brand or product identifier without hand-editing generated output.
5. Add smoke tests that launch the actual browser binary.
6. Document a fresh-checkout workflow and validate it on a clean worktree or CI environment.

Expected result: a reproducible Limitless-flavored Chromium binary that still behaves like Chromium and has not yet accumulated product UI complexity.

## STOP/RETHINK GATE 2: Integration seam proof

Stop before broad UI work unless a minimal vertical slice proves all of the following:

- the Limitless shell is constructed through the intended seam;
- an upstream `WebContents` can be hosted and navigated;
- browser commands and command enablement remain usable;
- focus can move correctly between shell and page content;
- native window dragging, resizing, controls, fullscreen, and close behavior work;
- one accessibility inspection shows meaningful roles, names, focus, and actions;
- the approach passes the supported Apple silicon macOS journey and preserves a
  source-backed boundary that does not gratuitously prevent a future port;
- patches remain narrow and product logic remains in `limitless/`.

If the seam fails one of these checks, fix or reconsider it before building more UI on top.

## Phase 2: Thin end-to-end browser slice

Build the smallest real user journey through the chosen architecture:

- launch the browser;
- create or restore a window;
- show one tab or workspace item;
- enter or choose a destination;
- navigate and display a page;
- show loading, title, URL, favicon, security, and error state as applicable;
- go back, forward, reload, and stop;
- open and close a second tab or equivalent Limitless navigation unit;
- close and reopen the browser with session behavior intact;
- open DevTools;
- complete core actions by keyboard only.

This slice must use real Chromium models and services. Do not use parallel mock state in the product UI except inside isolated tests.

Write E2E tests first for critical behavior where practical. At minimum, add a failing test or scripted reproduction before fixing defects discovered in the slice.

## STOP/RETHINK GATE 3: Product shell viability

Stop and review after the thin slice. Proceed only if:

- the UI state is derived from authoritative Chromium state;
- commands cannot silently drift from Chromium command enablement;
- no focus traps or keyboard dead ends remain;
- no security state is hidden or misleading;
- session and window lifecycle behavior is correct;
- native frames and scale factors behave on all target platforms tested so far;
- the shell can be tested without fragile coordinate-only automation;
- the patch manifest remains understandable and small;
- a Chromium rebase rehearsal does not reveal systemic coupling.

If the UI needs duplicated browser state, private implementation access across many components, or platform conditionals throughout shared product code, rethink the boundary now.

## Phase 3: Core browsing completeness

Expand by complete user journeys, not by isolated visual components. Suggested order:

1. Tab or workspace lifecycle, pinning/grouping if required, multiple windows, and session restore.
2. Address/search experience, autocomplete, security identity, and navigation errors.
3. History, bookmarks, downloads, find, zoom, print, save, and file interactions.
4. Permissions, content settings, authentication, certificates, password/autofill, and critical prompts.
5. Profiles, first-run, crash restore, settings, and migration behavior.
6. Extensions and extension surfaces if included in product scope.
7. DevTools, view source, task manager, and secondary browser windows.
8. Accessibility, localization, RTL, high contrast, reduced motion, and input-method hardening.

For every journey, define:

- authoritative Chromium model or service;
- Limitless presentation and interaction ownership;
- lifecycle and error states;
- macOS-specific behavior and deferred portability consequences;
- security and privacy consequences;
- E2E coverage;
- whether an upstream patch was added or expanded.

## Phase 4: Apple silicon macOS hardening

Run the complete behavioral suite on supported Apple silicon Macs, then add
focused macOS coverage for:

- native window controls, menus, shortcuts, fullscreen, and system dialogs;
- Retina and non-Retina mixed-display behavior and scale changes;
- GPU acceleration and software fallback;
- IME, dead keys, keyboard layouts, touch, trackpad, mouse, and drag-and-drop;
- VoiceOver, Accessibility Inspector, and native AX automation;
- sleep/wake, display changes, abrupt termination, crash recovery, and relaunch;
- installation, upgrade, profile-directory handling, and uninstall expectations;
- code signing, notarization, packaging, and update infrastructure when those enter scope.

Do not declare platform support based only on successful compilation.

## STOP/RETHINK GATE 4: Supported-Mac quality

Stop before release hardening if the Apple silicon Mac app cannot meet the
declared behavioral, accessibility, security, performance, and visual bar.
Reconsider abstractions if:

- shared code contains growing platform-condition branches;
- macOS requires a separate product interaction model without an explicit decision;
- accessibility differs materially across supported macOS versions or hardware;
- window behavior is emulated poorly instead of using appropriate native integration;
- failures cannot be reproduced and tested consistently;
- a feature requires unsafe or unmaintainable upstream changes on macOS.

Linux and Windows hardening becomes a separate owner-approved project only if
either platform enters product scope. Existing portability research is evidence,
not a v1 acceptance gate.

## Phase 5: Chromium update and release discipline

Design updates as a normal recurring workflow, not a future cleanup task.

For every Chromium revision update:

1. Record old and new full SHAs.
2. Review upstream security and breaking changes relevant to touched subsystems.
3. Apply the patch series with explicit failure on drift.
4. Build the supported Apple silicon macOS configurations.
5. Run unit, integration, browser, and E2E suites.
6. Manually inspect critical UI journeys on supported Macs.
7. Update architecture maps when ownership or seams change.
8. Record patch additions, removals, and conflict causes.
9. Rehearse profile and session compatibility where relevant.

Track metrics such as:

- number of upstream files touched;
- number and purpose of patches;
- patch churn per Chromium update;
- build and test duration;
- flaky test rate;
- crash-free critical journeys;
- platform-specific defect count;
- accessibility regressions;
- visual regression count.

An increasing upstream patch trend is an architecture warning, not just a maintenance statistic.

## E2E and quality expectations

### Test from the outside in

The primary proof is the built browser running as a user would run it. Unit tests and component tests are necessary but do not replace this proof.

Each core journey should have layers of coverage:

- focused unit tests for Limitless-owned state and adapters;
- integration or browser tests against real Chromium models and services;
- E2E tests launching the real Limitless browser binary;
- visual snapshots or screenshots for stable product surfaces where they provide useful signal;
- manual platform inspection for native behavior that automation does not assess reliably.

Prefer semantic automation hooks, accessibility identifiers, and observable browser state over screen coordinates and fixed delays. Treat test-only APIs that bypass real behavior with suspicion.

### Bug-fix workflow

For each bug:

1. Reproduce it in the real browser through the closest user journey.
2. Capture the conditions, platform, build, profile state, expected result, and actual result.
3. Add a failing E2E or highest-value automated regression test.
4. Trace the fault to the correct ownership layer.
5. Fix the root cause without expanding the upstream patch surface unnecessarily.
6. Run focused tests, then the relevant broader suite.
7. Inspect the rendered result carefully.
8. Check neighboring states and other platforms for the same class of defect.

### UI acceptance bar

Inspect at least:

- alignment, spacing, sizing, typography, icon rendering, and clipping;
- hover, pressed, selected, disabled, loading, error, and empty states;
- keyboard focus and focus restoration;
- animation timing, interruption, reduced motion, and jank;
- resize behavior and minimum window size;
- high-DPI and fractional scaling;
- light, dark, high-contrast, and forced-colors appearance;
- long titles, long URLs, localization expansion, and RTL;
- accessible names, roles, values, actions, and reading order.

If a clear defect is seen during E2E verification, address it even if it is adjacent to the immediate change, unless doing so would be risky. In that case, document the problem with reproducible evidence and stop for direction.

### Engineering acceptance bar

Before handing off a phase:

- formatting and lint checks pass;
- relevant unit, integration, browser, and E2E tests pass;
- no known flaky test is waved through;
- warnings introduced by the change are resolved;
- generated-file checks pass without manual generated-file edits;
- all required target platforms build at the phase's declared support level;
- documentation and patch manifests match the implementation;
- temporary experiments and dead code are removed;
- repository status contains only intentional changes.

## Security and privacy guardrails

Changes to visible browser chrome can affect whether a user correctly understands origin, permissions, downloads, authentication, and dangerous states. Therefore:

- never spoof, approximate, or cache security identity independently of Chromium's authoritative state;
- do not hide interstitials or warnings behind custom navigation;
- do not weaken sandbox, site isolation, process, or permission boundaries;
- keep privileged web-rendered UI behind explicit, narrow bindings if a web shell is chosen;
- validate every privileged message at the native boundary;
- expose only the capabilities the shell needs;
- threat-model navigation input, untrusted page content, drag-and-drop, clipboard, file access, extensions, and custom URL schemes;
- preserve clear trusted-versus-untrusted visual boundaries;
- require review for changes involving credentials, autofill, permissions, downloads, certificates, safe browsing, updates, or code execution.

## Decision log expectations

Create architecture decision records for choices that affect patch surface, process boundaries, UI technology, model ownership, cross-platform abstractions, security, accessibility, or update strategy.

Each record should include:

- context and decision drivers;
- options considered;
- decision;
- source evidence;
- consequences and risks;
- validation plan;
- conditions that would cause the decision to be revisited.

Do not let important architecture decisions exist only in chat history.

## How Codex should report progress

For each meaningful work session, report:

1. Outcome first.
2. Evidence inspected, including Chromium paths and symbols where relevant.
3. Files changed in `limitless/` and any upstream files changed in `chromium/src/`.
4. Tests and builds run, with results.
5. UI and platform behavior inspected.
6. Patch-surface change.
7. Risks, unknowns, and the next gate.

Do not say a phase is complete when only source code exists. Completion requires its declared tests, visual inspection, documentation, and gate criteria.

## Historical first Codex assignment

This assignment is complete and retained only to explain the origin of the
architecture documents. It is not the current continuation objective.

Start with Phase 0 only.

1. Verify the workspace layout and both recorded Chromium SHAs.
2. Read repository instructions and inspect existing Limitless code without altering user work.
3. Trace Chromium's desktop browser-window construction, model ownership, command routing, and cross-platform frame implementations at the pinned commit.
4. Evaluate the candidate UI architectures against the stated criteria.
5. Produce the required Phase 0 documents with exact source evidence.
6. Recommend one architecture and a thin vertical slice.
7. Stop at Gate 1 and ask for architecture approval.

Do not begin broad implementation, switch Chromium revisions, or make large upstream patches during this assignment.

## Historical first-session completion checklist

Phase 0 is ready for review only when:

- [ ] the Chromium checkout and `chromium.version` match the pinned full SHA;
- [ ] applicable agent instructions and repository state were reviewed;
- [ ] the top-level window and UI ownership path is mapped with exact source symbols;
- [ ] browser capabilities are separated from their current presentation layers;
- [ ] all candidate architectures have an evidence-backed comparison;
- [ ] macOS, Linux, and Windows implications are explicit;
- [ ] non-main-window Chromium UI surfaces are inventoried;
- [ ] build integration and patch strategy are documented;
- [ ] security and accessibility risks are documented;
- [ ] the expected initial patch surface is listed;
- [ ] a thin end-to-end slice and its tests are proposed;
- [ ] Gate 1 questions and stop conditions are presented to the user;
- [ ] no production architecture was silently committed before approval.
