# Patching Chromium in Limitless

## Purpose

This is the operating guide for every Limitless change to upstream Chromium. It
defines how to decide whether a patch is justified, where the change belongs,
what evidence must accompany it, and how the patch is maintained across Chromium
updates.

Treat Chromium as a pinned upstream dependency. A Chromium patch is a temporary
compatibility layer between that dependency and Limitless, not the natural home
for product code.

The exact approved patch inventory, watched symbols, and growth limits live in
the [Chromium patch surface](architecture/patch-surface.md). Repository and build
mechanics live in [Build and repository integration](architecture/build-integration.md).
The architectural boundary is recorded in
[ADR-0001](architecture/decision-record.md).

Some patch directories and tools described here are Phase 1 deliverables and do
not exist yet. Their contracts are still requirements. Do not substitute an
undocumented manual workflow when a required tool has not been implemented.

## Goals

- keep Chromium updates fast, predictable, and reviewable;
- keep Limitless behavior, styling, and product policy in the Limitless repository;
- make every upstream change narrow enough to understand in isolation;
- fail clearly when upstream behavior invalidates a Limitless assumption;
- preserve Chromium's security, process, navigation, and platform boundaries;
- make patches removable when Chromium gains an adequate extension point.

The objective is not the lowest line count at any cost. Prefer the simplest
stable ownership boundary with the lowest long-term coupling.

## Modification decision order

Use the first durable option that can satisfy the requirement:

1. **Limitless-owned implementation.** Put product C++, TypeScript, CSS,
   resources, build targets, tests, and policy in `limitless/`.
2. **Existing Chromium seam.** Use a supported delegate, observer, controller,
   service, feature flag, registration point, model API, or GN argument.
3. **Small generic hook.** Add the narrowest upstream hook that lets
   Limitless-owned code participate. Prefer a hook that could reasonably be
   contributed upstream.
4. **Maintained direct patch.** Patch an upstream implementation only when the
   preceding options cannot preserve the intended architecture.
5. **Architecture review.** Stop before copying an upstream implementation,
   altering a security boundary, or spreading product branches through Chromium.

Line count is only one signal. A three-line patch in a fragile global lifecycle
can be riskier than a small, well-scoped interface in a stable ownership seam.

## Limitless is not `brave-core`

Brave's patching guide provides useful principles, especially keeping product
code separate, preferring narrow overrides, and using direct patches last.
Limitless uses a different integration design:

- Limitless-owned sources remain in the sibling `limitless/` repository.
- Chromium reaches them through explicit GN system-absolute labels.
- Upstream changes are an ordered, hashed patch series.
- Limitless does not currently provide a `chromium_src` replacement layer.
- Limitless does not use Brave's pnpm patch-generation commands.

Do not introduce a `chromium_src`-style overlay merely to imitate Brave. Such a
mechanism would be a separate architecture decision with its own build,
correctness, and stale-code risks.

Never copy a complete Chromium source file into Limitless to hide a small
product difference. A copied implementation stops receiving upstream fixes
without producing an obvious patch conflict. If replacing a whole file appears
necessary, stop and update the architecture decision before implementation.

## What may belong in a Chromium patch

A patch may contain:

- a narrow build-graph hook for a Limitless-owned target;
- a factory, registration, or resource-selection seam;
- a small lifecycle call into a Limitless-owned adapter;
- a generic upstream bug fix with a regression test;
- a temporary compatibility change for a pinned Chromium transition.

A patch must not contain:

- Limitless styling, layout, copy, icons, or product business logic;
- a copied Chromium class or complete implementation;
- generated sources, generated resources, build output, or `CHANGELOG.md`;
- unrelated formatting, cleanup, or refactoring;
- disabled security checks or weaker sandbox, site-isolation, permission,
  certificate, Safe Browsing, or navigation behavior;
- several independent changes combined for convenience.

If a generic Chromium defect blocks Limitless, prefer fixing the defect in a
form suitable for upstream review. Keep the local patch only until the fix is
available in the pinned Chromium revision.

## Required evidence before authoring a patch

Before changing an upstream file, record:

1. **Pinned base.** The exact SHA in `chromium.version` and the matching
   `chromium/src` SHA.
2. **User-visible requirement.** The end-user journey or invariant the change
   enables.
3. **Reproduction.** For a bug, reproduce the failure in the built browser as
   closely as possible to the way a user encounters it.
4. **Source trace.** Identify the owning upstream files, symbols, lifecycle, and
   existing extension points.
5. **Rejected seams.** Explain why Limitless-owned code and existing Chromium
   seams are insufficient.
6. **Proposed boundary.** Identify the Limitless target that will own the actual
   behavior.
7. **Test plan.** Name the narrow regression test and the relevant end-to-end
   journey.
8. **Conflict signals.** List upstream symbol or behavior changes that require
   review even if the patch still applies.
9. **Removal path.** State what upstream change would allow the patch to be
   deleted.

Do not use a speculative patch to skip source analysis. A disposable experiment
may answer a specific unknown, but it does not become production architecture
without the evidence above.

## Patch repository layout

The intended canonical layout is:

```text
limitless/
  chromium.version
  patches/chromium/
    manifest.json
    series
    0001-area-purpose.patch
    0002-area-purpose.patch
  tools/chromium/
    verify
    apply-patches
  docs/
    chromium-patching-guide.md
    architecture/patch-surface.md
```

`series` contains one patch filename per line in application order. Use names of
the form `NNNN-area-purpose.patch`. Keep a name stable while the patch preserves
the same intent. Split the patch when its responsibilities become independent.

`manifest.json` is machine-readable authority for the patch series. For each
patch it must record at least:

| Field | Requirement |
| --- | --- |
| Base | Exact Chromium SHA |
| Integrity | Patch filename and SHA-256 |
| Surface | Upstream files and symbols touched |
| Reason | Requirement and why no existing seam is sufficient |
| Owner | Dependent Limitless target or component |
| Tests | Required unit, browser, UI, and end-to-end coverage |
| Drift | Expected conflicts and watched behavioral signals |
| Exit | Removal or upstreaming condition |

The manifest and `series` must agree. An unlisted patch, missing patch, changed
hash, unexpected touched file, or wrong base is a hard failure.

## Authoring workflow

### 1. Verify the baseline

Run the repository verification tool before editing. It must compare the current
Chromium SHA with `chromium.version`, report both worktree states, and refuse to
continue when the base or existing patch state is ambiguous.

Verification never resets, cleans, fetches, switches, or discards work. Resolve
unexpected local state deliberately. Never overwrite unrelated work.

### 2. Establish the test first

For a bug fix, capture the failing end-user journey before changing code. Add the
closest automated reproduction available, preferring this order:

1. outside-in Limitless browser smoke or end-to-end test;
2. Chromium browser or interactive UI test;
3. focused unit test for the narrow underlying contract.

Use lower-level coverage in addition to, not as a replacement for, the user
journey when the bug crosses browser UI, renderer, process, or platform
boundaries.

For a new integration seam, define a falsifiable proof before patching. Examples
include selecting the intended window implementation, loading a unique
Limitless resource, or invoking a product adapter without changing ordinary
Chromium behavior.

### 3. Make one coherent upstream change

Keep the patch focused on one ownership boundary. Product behavior stays behind
the hook in a Limitless target. Avoid opportunistic cleanup and avoid moving
unrelated upstream code.

Where possible, make ordinary Chromium behavior byte-for-byte or behaviorally
unchanged when the Limitless build argument or compile condition is absent.

### 4. Validate the changed browser

At minimum:

- build the real browser target containing the change;
- run the narrow unit, browser, or interactive UI regression test;
- run the relevant Limitless end-to-end journey with a fresh test profile;
- inspect visible output, focus, accessibility, input, and native window behavior;
- run the relevant lint and build-graph checks;
- verify the upstream-default configuration still builds or generates when the
  Limitless integration is disabled.

Platform-sensitive v1 changes require the applicable macOS checks from the
[platform matrix](architecture/cross-platform-matrix.md). Linux and Windows
checks become mandatory only if either platform is separately approved for
support after v1. Do not treat a compilation-only check as evidence for UI,
focus, accessibility, or lifecycle correctness.

### 5. Generate and describe the patch

Generate the patch against the exact recorded base using project tooling. Do not
manually edit a generated patch after its checksum is recorded. Update the
manifest entry, add the filename to `series`, and update
`architecture/patch-surface.md` when the approved surface changes.

The patch description should answer:

- what stable seam it introduces or repairs;
- why the implementation remains Limitless-owned;
- what happens in a non-Limitless Chromium build;
- which test fails if the contract breaks;
- how and when the patch can be removed.

### 6. Prove clean application

Use `tools/chromium/apply-patches --check` on a clean checkout at the exact pin,
then apply the complete ordered series in a disposable or CI checkout. The
application tool must:

- reject the wrong Chromium SHA;
- validate manifest and patch hashes;
- reject unexpected pre-existing changes;
- check every patch before mutating the checkout;
- refuse three-way fallback, fuzz, and silent offset acceptance;
- verify the resulting touched-file set.

Routine development and CI must use the tool, not an undocumented sequence of
manual `git apply` commands.

### 7. Review the resulting product

Review the patch diff and the running Limitless browser. A green narrow test does
not excuse visible regressions, broken keyboard behavior, inaccessible controls,
new warnings, lint failures, or flaky related tests.

## Patch review checklist

A Chromium patch is ready only when every applicable item is true:

- [ ] It has one coherent purpose.
- [ ] The exact Chromium base is recorded.
- [ ] Upstream files and symbols are listed.
- [ ] The user journey or architectural invariant is documented.
- [ ] Existing Chromium seams were investigated first.
- [ ] Product behavior remains in Limitless-owned code.
- [ ] No complete upstream implementation is copied.
- [ ] No generated file or `CHANGELOG.md` is changed.
- [ ] Default Chromium behavior is preserved when Limitless is disabled.
- [ ] Security and process boundaries are unchanged or explicitly reviewed.
- [ ] A narrow automated regression test exists.
- [ ] The relevant end-to-end journey passes.
- [ ] UI, focus, input, accessibility, and platform behavior were inspected.
- [ ] Conflict signals and watched symbols are recorded.
- [ ] A removal or upstreaming path is recorded.
- [ ] The patch applies through the strict tool on a clean exact-base checkout.
- [ ] The manifest hash and touched-file set match.

## Chromium update workflow

Prepare an update in a clean branch or checkout rather than mutating the current
pinned workspace in place.

1. Record the old and proposed full Chromium SHAs.
2. Sync a clean checkout to the proposed SHA.
3. Run the complete patch series in read-only check mode.
4. Inspect changes to every watched symbol, even when all patches apply.
5. Re-evaluate whether upstream added a seam that makes any patch removable.
6. Resolve one patch at a time, preserving its documented intent rather than its
   old diff shape.
7. Update its tests, conflict signals, manifest entry, and checksum.
8. Generate and build the supported platform configurations.
9. Run unit, browser, interactive UI, end-to-end, accessibility, and visual
   checks required by the affected surface.
10. Record patch count, touched-file count, patch churn, conflicts, removals, and
    disabled-test changes.
11. Change `chromium.version` only after the complete update passes.

When a patch conflicts, do not immediately recreate the old edit at a new line.
First reread the surrounding ownership and lifecycle. A conflict may indicate
that the upstream contract moved, a better extension point now exists, or the
Limitless assumption is no longer valid.

When a patch applies cleanly, do not assume it remains correct. Silent semantic
drift is why the manifest includes watched symbols and behavioral tests.

## Stop conditions

Pause patch authoring and revisit the architecture when:

- the change requires copying or replacing a complete Chromium implementation;
- product logic must appear in more than a narrow hook;
- a patch reaches sandbox, site isolation, navigation security, permission,
  certificate, Safe Browsing, or network ownership;
- different platforms begin accumulating independent product patch sets;
- the browser-window factory needs a product-specific parallel hierarchy;
- repeated updates preserve application but break behavior;
- the patch surface exceeds the limits in
  [Chromium patch surface](architecture/patch-surface.md).

Development cost is not a reason to ignore a stop condition. Revisit the seam,
upstream a generic hook, or change the product architecture.

## Current Limitless boundary

At the current architecture gate, Limitless retains the earlier
`WebUIBrowserWindow` and SurfaceEmbed evidence while evaluating a
BrowserView-hosted SwiftUI candidate. Product UI, tab projection, Herdr presence,
Leader state, command mapping, policy, and tests remain in Limitless-owned
targets. The series does not patch the browser-window factory or introduce a
Limitless `BrowserWindow` subclass.

Patch `0006-browser-view-leader-shortcuts.patch` adds two conditional adapter
calls to existing Chromium lifecycle and input seams:

- `BrowserView::PreHandleKeyboardEvent` offers renderer-originated keyboard
  events to the exact window's product-owned Leader controller before Chromium
  continues its normal handling;
- `BrowserNativeWidgetMac::OnWindowInitialized` asks a narrowly named
  product-owned factory for an exact-window command-dispatcher delegate and
  installs Chromium's stock delegate when the product is disabled or the window
  is ineligible.

The patch contains no shortcut map, tab mutation, timer, policy, or SwiftUI
presentation logic. Renderer, Views-control, and in-process APPLICATION tests
exercise the product-owned routes, but external hardware input, IME, AX, and
pixel acceptance remain separate red gates. Any expansion beyond these two
watched symbols requires a fresh patch-surface review.

The current and expected production patches are specified only in
`architecture/patch-surface.md`. If implementation discovers another required
upstream file, update and review that document before expanding the series.

## Origin and further reading

This guide adapts the durable ideas in Brave's patching practice to Limitless's
separate repository, external GN target, exact Chromium pin, and hashed patch
series. It does not copy Brave's repository-specific workflow.

- [Brave: Patching Chromium](https://github.com/brave/brave-core/blob/master/docs/patching_and_chromium_src.md)
- [Revision-pinned Brave guide](https://github.com/brave/brave-core/blob/323b9e5ef2b76dcb77961d9f38d9d33881fe1488/docs/patching_and_chromium_src.md)

The most reusable lessons are to keep product changes outside upstream code,
prefer extension mechanisms over direct patches, make unavoidable patches
trivial and explicit, and avoid whole-file copies that silently go stale.
