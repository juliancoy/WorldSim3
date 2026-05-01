# TODO

## Immediate Unresolved Issues

- Verify vacant parcel overlay visibility on a fresh app restart using the latest binary:
  - Enable `Vacant Building Notices` and/or `Vacant Building Rehabs`.
  - Confirm matched parcel outlines appear before full triangulation completes.
  - Confirm filled overlay appears once parcel triangulation is ready.
- Confirm hydration queue behavior for disabled layers in all edge cases:
  - Uncheck while queued.
  - Uncheck mid-hydration.
  - Re-enable after stop.
- Confirm parcel dependency hydration works when only vacant layers are enabled and parcel layer is hidden.

## REST / Observability

- [x] Validate `/status` payload in production runs includes:
  - `hydration_pct`
  - `triangulation_pct`
  - per-layer `hydrated` and `triangulated` booleans
- [x] Add API schema/version field for status contract stability.
- [x] Add elapsed time + throughput metrics to `/status` (layers/min, features/sec).

## Performance Work (From Live Profiling)

- [x] Add geometry LOD/simplification for heavy polygon layers (especially parcels) at low zoom.
- [x] Add cached transformed screen-space geometry per zoom/tile bucket to reduce repeated `worldToScreen` work.
- Move vacancy overlay to a precomputed derived layer cache (`parcel_vacancy_status`) to avoid repeated per-frame joins.
- [x] Add profiling mode toggle and frame-time counters in UI and `/status`.

## Data Pipeline / Quality

- [x] Build a persistent derived dataset:
  - `data/cache/derived/parcel_vacancy_status.*`
  - fields: `blocklot_norm`, `vacant_notice_count`, `vacant_rehab_count`, `vacancy_weight`, join diagnostics.
- [x] Emit join diagnostics and unmatched key reports:
  - total rows, matched rows, unmatched rows, top unmatched examples.
- Add fallback spatial join for records missing/invalid `BLOCKLOT`.

## Rendering / UX

- Add legend for vacancy overlay intensity and what counts are included.
- Add separate toggles:
  - raw vacant points (debug)
  - vacancy-by-parcel derived overlay (primary)
- Add click-to-pin parcel inspector panel (persistent details while panning).

## Caching / Startup

- Validate hydration cache integrity across file changes and partial writes.
- Add cache format version migration handling and explicit invalidation command.
- Optionally compress hydration cache files if disk footprint grows too large.

## Test / Verification

- Add repeatable perf benchmark script (startup time, time-to-first-map, hydration completion time).
- Add regression checks for:
  - point geometry ingestion
  - unchecked-layer hydration policy
  - UI state persistence (`data/layer_ui_state.json`)
  - vacant-to-parcel join normalization.
