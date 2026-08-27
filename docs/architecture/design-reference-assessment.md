# Arc and Mori design-reference assessment

## Decision

Use Arc and Mori as product and interaction references, not as architecture authorities. `docs/features.md` is the product authority and now explicitly selects the Arc-style sidebar, workspaces, command palette, Peek, split browsing, sharp visual language, fast motion, and related v1 requirements.

- Arc has the higher value as a product-design reference. The user's concrete Arc learnings should refine the explicit Limitless requirements into measurable behavior.
- Mori has value as a working macOS interaction study and as evidence that a small team can explore opinionated browser workflows quickly.
- Do not base Limitless on Mori's window, state, content-hosting, build, or test architecture.
- Do not infer unlisted Arc or Mori behavior. Implement the behaviors selected in `docs/features.md` and `docs/v1-product-roadmap.md`, then use reference products to sharpen their interaction quality and acceptance criteria.

This separation matters because a good interaction can be reproduced on a safer ownership boundary. Conversely, a visually lean repository can hide a broad and fragile Chromium integration.

## Arc learning intake

The user's Arc experience is primary evidence. Capture each learning in a product-design brief using this format:

| Field | Question |
| --- | --- |
| User problem | What recurring browsing problem did Arc solve or expose? |
| Arc behavior | What exact interaction produced the learning? |
| Durable principle | What should remain true even if Limitless looks and behaves differently? |
| Failure or tradeoff | What became confusing, slow, hidden, fragile, or overly opinionated? |
| Limitless hypothesis | What behavior should Limitless test? |
| Chromium authority | Which Chromium model or service remains the source of truth? |
| macOS consequence | Which menu, window, focus, drag, accessibility, or native convention is involved? |
| Acceptance evidence | What user journey or prototype result would prove the hypothesis? |
| v1 priority | Required, candidate, or deferred? |

The product direction now explicitly includes vertical navigation, workspaces, command-first operation, a dense sidebar, Peek, split views, Herdr-like keyboard behavior, gradient themes, fast motion, and a sharp visual hierarchy. Design work should define the detailed behavior and tradeoffs for those selected requirements. Onboarding, automatic archiving, and other unlisted Arc behavior remain proposals until `docs/features.md` selects them.

## Mori snapshot inspected

Repository: [FujiwaraChoki/mori-browser](https://github.com/FujiwaraChoki/mori-browser)

Inspected revision:

```text
b5b29c2e29061f9b2009e8cc0b2756c6ad62bb20
release: bump Mori to 2.0.5
2026-07-08T20:19:31+02:00
```

The revision contains 110 tracked files and about 27,000 lines of first-party Swift, Objective-C++, and header code under `chrome/browser/ui/mori`. It uses an MIT license. The line count describes source volume, not quality or completeness.

## Useful Mori lessons

### Product and interaction ideas worth evaluating

- A macOS-only product can explore native-feeling browser interactions much faster than a simultaneous three-platform release.
- Spaces or contexts, folders, pinned items, Peek, split browsing, host-based routing, sleep/archive behavior, and a unified launcher are concrete hypotheses worth testing against the user's Arc learnings.
- A single shortcut registry is a good product rule. Browser command authority and macOS menu validation must still remain in Chromium.
- Treating page content as independent from product chrome is directionally correct.
- Explicitly documenting experimental security risks, especially injected scripts and AI page automation, is good practice.
- Native macOS behavior should be tested as a product capability: traffic lights, application menus, first-responder changes, fullscreen, sheets, services, VoiceOver, and display movement.

### Engineering practices worth adapting

- Keep product UI and native integration conceptually separate.
- Use small feature-specific stores and views where the state is genuinely product-owned.
- Give transient browsing, tab routing, and resource lifecycle explicit models rather than scattering flags through UI components.
- Preserve a dedicated isolated profile and a repeatable launch command during browser development.

These ideas should be adapted to Limitless's authoritative Chromium boundary, patch manifest, and outside-in test requirements rather than copied structurally.

## Mori architecture we should not adopt

### Third `BrowserWindow` implementation

`mori_browser_window.h` defines `MoriBrowserWindow : BrowserWindow`, plus substitute `MoriFindBar` and `MoriLocationBar` implementations. `mori_browser_window.mm` contains many no-op methods and default `false`, empty, or `nullptr` returns for the broad browser-window contract.

This is the exact route rejected by Limitless Phase 0. At the pinned Limitless Chromium revision, browser features already have explicit `BrowserView` and `WebUIBrowserWindow` branches. A third implementation must continuously chase that interface and the concrete feature-controller assumptions around it.

### Parallel browser state and session authority

`BrowserStore.swift` is about 1,900 lines and writes its own `session.json`. It owns a parallel tab list, selection, contexts, pinned state, closed-tab sessions, sleep/archive behavior, and restore flow, then mirrors selected state into `TabStripModel`.

Limitless must instead derive browser lifecycle and session truth from Chromium. Product-only organization may be persisted separately, but it must reference authoritative Chromium tab, session, profile, and navigation identities and define reconciliation behavior explicitly.

### Direct native-view and delegate manipulation

`mori_chrome_bridge.mm` is over 2,100 lines. It directly obtains the native NSView from each tab `WebContents`, reparents it into SwiftUI/AppKit containers, swaps delegates, tracks global maps, and uses an orphan map keyed by URL to adopt engine-created tabs.

That can produce a compelling prototype, but URL-keyed adoption, delegate replacement, and direct native-view ownership are high-risk lifecycle seams. Limitless should first validate the upstream SurfaceEmbed path, where Chromium keeps the independent `WebContents` ownership model and explicit attachment lifecycle.

### Global single-window assumptions

The bridge stores one global `Browser*` and one global `NSWindow*`, and the first normal browser becomes the Mori browser. This is incompatible with the Limitless requirement to preserve multiple windows, multiple profiles, incognito separation, correct command targeting, and independent close/session lifecycles.

### Incomplete reproducibility and testing

The repository does not track:

- an exact Chromium or ungoogled-chromium revision;
- the modified Chromium `chrome/browser/ui/BUILD.gn` that its documentation says is required;
- a Chromium patch manifest or patch files;
- deterministic checkout and GN-generation scripts;
- automated unit, browser, interactive UI, E2E, or accessibility tests;
- CI workflows.

The build guide assumes an existing local Chromium checkout and generated `out/Default`. Therefore the public repository's small footprint is not evidence of a small, reproducible upstream patch surface.

### Security-sensitive scope

Mori's own README classifies it as experimental and calls out injected CSS/JavaScript, AI page-reading, and prompt-injection risks. Limitless should not use page scripting as a shortcut for browser integration. Any later AI or site-customization capability needs an explicit threat model, scoped consent, origin isolation, auditability, and tests against malicious page content.

## Code-reuse policy

Do not copy Mori implementation code into the initial Limitless foundation. The architecture and state boundaries differ too much for code reuse to be the low-risk choice.

If a small, self-contained macOS interaction or visual utility is later worth reusing:

1. verify that it does not depend on Mori's global browser, parallel session model, or direct `WebContents` view ownership;
2. review its security, accessibility, and performance behavior independently;
3. preserve the MIT license notice and record the source revision in third-party notices;
4. rewrite the integration against Limitless's typed Chromium boundary;
5. add end-to-end and focused regression coverage before shipping it.

## Recommendation

Borrow product questions aggressively and implementation patterns selectively:

| Reference | Use it for | Do not use it for |
| --- | --- | --- |
| Arc | User problems, interaction principles, selected information architecture, polish bar, and known tradeoffs | Requirements beyond `docs/features.md`, copied visual identity, or assumed implementation details |
| Mori | Concrete macOS interaction experiments, feature discovery, native-behavior test cases, and evidence of prototype velocity | BrowserWindow architecture, authoritative state, content attachment, patch strategy, testing, or release process |

The next design step is to turn the selected product vision into measured interaction specifications and approved pixel references. The next engineering step remains the narrow macOS Gate 2 integration proof defined in `decision-record.md`, including the native SwiftUI comparison now required by the product roadmap.
