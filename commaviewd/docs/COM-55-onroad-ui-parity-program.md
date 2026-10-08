# COM-55 — comma-device onroad UI parity program plan

> Historical program plan. The telemetry JSON milestone and master-only execution rule
> predate the current runtime and repository workflow. Use the [current documentation](../../docs/README.md)
> and [AGENTS.md](../../AGENTS.md) for today's behavior and change process.

## Objective
Deliver testable onroad UI parity against sunnypilot staging baseline `33a8902` with phased milestones and evidence gates.

## Scope
- `commaviewd` (bridge/runtime telemetry + camera/calibration contract surface)
- `CommaView` app repo (parser + rendering/UI behavior)

This ticket is the umbrella execution plan. Implementation details stay in milestone child issues.

## Milestones

### M1 — Telemetry contract parity (bridge + app parser)
**Goal:** Make runtime telemetry payload fully match upstream behavior needed by app overlays.

**commaviewd deliverables**
- Lock telemetry JSON schema fields required by onroad widgets/HUD.
- Add/maintain schema parity tests and strict failure on field drift.
- Keep source attribution comments for upstream-derived semantics.

**Evidence gate**
- `commaviewd/tests/test_telemetry_json.cpp` covers required fields and nullability/default behavior.
- Unit tests pass in CI and local run.

### M2 — Camera + calibration parity
**Goal:** Match camera framing/calibration inputs expected by upstream-style rendering.

**commaviewd deliverables**
- Validate camera metadata and calibration values forwarded to app.
- Confirm wide/road/driver stream metadata consistency and ordering.

**Evidence gate**
- Contract checks include calibration and camera-path sanity.
- Captured telemetry samples verify parity with upstream reference sessions.

### M3 — HUD + torque + driver widget parity
**Goal:** Ensure runtime emits all state needed for HUD/torque/driver widgets with matching timing.

**commaviewd deliverables**
- Verify control/state fields that feed widget rendering are present and fresh.
- Harden stale-data handling so app can reject/gray invalid overlays deterministically.

**Evidence gate**
- Added/updated tests for freshness windows + fallback behavior.
- On-device capture/replay demonstrates matching widget transitions.

### M4 — Fade/border/render-order parity
**Goal:** Preserve layer ordering and transition timing contracts used by app rendering.

**commaviewd deliverables**
- Ensure event timing and per-frame fields are stable enough for deterministic draw ordering.
- Document any non-upstream deltas explicitly (temporary waivers only).

**Evidence gate**
- Side-by-side capture checklist signed off.
- No legacy overlay path dependencies left enabled.

### M5 — Recording parity + final validation
**Goal:** Validate end-to-end parity in recorded and live runs before closeout.

**commaviewd deliverables**
- Record/replay parity sanity for telemetry + stream alignment.
- Final verification report with pass/fail by onroad scenario.

**Evidence gate**
- Local build + unit tests green.
- Milestone evidence links attached in Linear before issue close.

## Execution rules
- Master-only workflow (no feature branches).
- Keep each child milestone PR/commit scoped to one parity slice.
- Attach command output and artifact paths for every milestone gate.
- Do not ship parity changes without corresponding tests.

## Verification baseline commands
Run from repo root:

```bash
commaviewd/scripts/build-ubuntu.sh
commaviewd/scripts/run-unit-tests.sh
```

## Done criteria for COM-55
- All five milestones completed with evidence.
- Upstream parity captures reviewed for key onroad states.
- No stale/legacy overlay paths remain.
- Final validation summary attached to the issue.

## COM-57 telemetry contract test command
Run from repo root:

```bash
commaviewd/scripts/run-unit-tests.sh
```

This includes `tests/test_telemetry_json.cpp` coverage for new payload service types and fallback/default behavior checks.
