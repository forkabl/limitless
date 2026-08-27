# Shared product control planes

Status: foundation checkpoint. `docs/features.md` remains the product authority.

The control-plane milestone starts with two product-owned, typed boundaries:

- `shell/control_plane` defines a versioned preference request envelope for
  CLI and agent adapters. The first key is `rail_density`, with `compact` and
  `comfortable` values. Requests use canonical opaque request, profile, and
  profile-instance UUIDs, a bounded actor identity, and an explicit capability
  credential. Parsing rejects unknown fields, unsupported versions, malformed
  identifiers, invalid operations, and out-of-range values.
- `shell/commands` defines stable product command names and typed arguments for
  Leader, native UI, command palette, CLI, and agent adapters. Split targets
  contain canonical product UUIDs only. The registry owns descriptors and
  dispatch callbacks, but no browser, tab, page, workspace, split, or enabled
  state. Every execution re-queries live authority before dispatch.

The preference service authenticates before resolving its target and rejects
profile mismatch, private profiles, stale profile instances, managed writes,
and backend write failures. Effective values are read back from the backend,
never echoed from the request. Every typed request outcome is sent to a bounded
audit sink without recording credentials, paths, WebContents, or raw handles.

This checkpoint intentionally does not claim the running-profile journey. The
next slice supplies the registered non-sync Chromium profile preference,
opaque profile identity and capability issuance, live `PrefChangeRegistrar`
projection into the native SwiftUI rail, clean restart proof, and a real
running-process CLI or agent transport. Malformed transport requests will be
audited at that adapter boundary. Leader and split host routes remain unchanged
until the registry receives exact-window Chromium-backed query and execution
adapters.

The design keeps Chromium authoritative for browser commands, tab order,
workspace membership, and split state. Product control planes carry typed
intent and result status only; they do not mirror browser state or expose raw
Chromium identifiers.
