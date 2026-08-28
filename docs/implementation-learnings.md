# Limitless implementation learnings

## Outcome

The implementation through commit `6727ed1` was rejected by the product owner.
It accumulated substantial browser infrastructure, but it did not deliver a
visually convincing, frequently testable expression of the product vision soon
enough. The code has been removed from the working tree while the documentation
and Git history are retained.

## Product direction that remains authoritative

`docs/features.md` remains the source of truth. The compact statement of the
vision is:

> Limitless should look like Arc, use sharper corners, and have Herdr-like
> shortcuts.

The product should feel lean, fast, compact, browser-native, and friendly to AI
control without putting AI in the user's face. It should not look like ordinary
Chrome with an added sidebar.

## What went wrong

1. Infrastructure got too far ahead of product proof. Large amounts of work
   went into lifecycle safety, command targeting, transport, and adversarial
   tests before the overall browser appearance and interaction model had earned
   approval.
2. Testable builds were not delivered at a useful cadence. Green engineering
   gates did not translate into regular checkpoints the product owner could
   open, judge, and redirect.
3. Progress reporting overemphasized internal milestones. Passing tests and
   hardened registries are not meaningful product progress when the visible
   browser still does not feel right.
4. The effort did not establish an approved visual reference early enough.
   Arc-inspired layout, sharp geometry, spacing, animation, sidebar behavior,
   and browser chrome should have been demonstrated together in a small
   end-to-end prototype before deeper platform work.
5. Time estimates were repeatedly presented as short rolling estimates even
   though the remaining source-of-truth scope was much larger. Future estimates
   should be tied to named acceptance gates and include uncertainty.
6. The work optimized for robustness within each engineering slice, but the
   ordering did not optimize for early validation of the owner's taste and
   product thesis.

## Technical findings worth retaining

1. A BrowserView-hosted AppKit and SwiftUI composition can project live Chromium
   state without creating a second browser model. Chromium should continue to
   own tabs, navigation, profiles, sessions, WebContents, and security-sensitive
   surfaces.
2. Product workspaces can reference stable product UUIDs while resolving live
   Chromium tab handles at use time. Persisting duplicate URLs, titles,
   WebContents, or split state creates avoidable consistency problems.
3. Chromium's native two-tab split substrate can support create, focus, layout,
   ratio, swap, unsplit, restoration, and bounded transfer behavior. Limitless
   should not create a parallel split authority.
4. Per-window command authority needs both persistent and ephemeral identities.
   Exact profile and window instance targeting prevents commands from silently
   falling back to whichever window happens to be active.
5. Callbacks can unregister, replace, or destroy their owning registry. Safe
   command dispatch requires copied tokens, weak ownership, identity
   revalidation after callbacks, and explicit fail-closed results.
6. A single typed command plane remains a sound long-term boundary for native
   controls, shortcuts, a command palette, CLI tools, and authorized agents.
   It should be introduced only as demanded by approved user-facing journeys.
7. A typed preference plane with profile scoping, policy checks, effective-value
   reporting, authorization, and auditing is preferable to exposing arbitrary
   Chromium preferences.
8. macOS component builds emit duplicate Objective-C class warnings from
   upstream Chromium library topology. Production packaging should validate a
   non-component build instead of hiding those warnings in product code.
9. External hardware input, IME, VoiceOver, accessibility inspection, native
   dragging, fullscreen, and Retina screenshots cannot be replaced by
   in-process simulations. They must remain explicit release gates.

## Better restart sequence

1. Turn `features.md` into a visible acceptance checklist without narrowing it.
2. Produce one high-fidelity static or lightly interactive browser prototype
   showing the full Arc-like composition, sharper corners, workspace sidebar,
   agent area, split affordances, command palette, and navigation chrome.
3. Get explicit approval of the visual direction at normal and minimum window
   sizes before building browser infrastructure.
4. Build one disposable but real app journey: launch, navigate, open tabs,
   switch workspace, invoke a Leader shortcut, split two pages, quit, and
   restore.
5. Give the product owner a runnable build at the end of every visible slice.
   Stop and redirect immediately when the experience is rejected.
6. Add deeper command, transport, authorization, and lifecycle infrastructure
   only behind an already approved user-facing need.
7. Keep each checkpoint committed, but report progress primarily in terms of
   what the owner can now see and do.

## Recovery and retained state

- The rejected implementation remains recoverable in Git history through
  commit `6727ed1` on `phase-2/external-control-transport`.
- The sibling Chromium checkout was not deleted.
- Documentation, including the original feature vision and architecture
  research, is retained.
