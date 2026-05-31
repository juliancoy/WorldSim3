# Improvement Plan

## Parcel Hover and Click Lookup

- Use `unified_parcels` as the authoritative parcel summary and identity source
  for hover, click, selection, filters, and high-traffic detail fields.
- Use parcel relationship artifacts for related property records, events, and
  source features that are associated with a parcel but should not be flattened
  into the summary row.
- Avoid falling back to raw `parcel_features` for parcel identity or details.
- Replace manual `DuckDbQueryResult` row parsing in UI code with a typed API, such as:
  - `DuckDbAnalytics::queryUnifiedParcelRecord(entity_id)`
  - `DuckDbAnalytics::queryUnifiedParcelDetailRecord(entity_id)`
- Make hover and click call the same resolver so behavior cannot drift.
- Keep geometry entity IDs and canonical parcel entity IDs explicit in resolver outputs.

## DuckDB Readiness

- Centralize readiness checks behind a single method such as `DuckDbAnalytics::ready()` or `DuckDbAnalytics::ensureReady()`.
- Avoid direct scattered checks of `status().last_rebuild_ok`.
- Revalidate after transient lock failures, but throttle revalidation so UI hover paths do not repeatedly open DuckDB.
- Preserve a useful diagnostic message when readiness fails, including whether the issue is missing cache, stale schema, lock contention, or failed validation.
- Prefer read-only DuckDB connections for lookup/query paths that do not mutate analytics state.

## Process and Lock Safety

- Add a single-instance guard or lockfile for the GUI process to prevent multiple `worldsim3` instances from fighting over `worldsim.duckdb`.
- If multiple instances must be supported, assign separate REST ports and use read-only DuckDB connections where possible.
- Surface DB lock contention clearly in the UI/status API instead of silently degrading parcel details.
- Ensure tests and scripts clean up spawned app processes reliably.

## Polygon Choropleth Rendering

- Treat any polygon layer with a numeric `heatmap_field` as a continuous choropleth candidate, not only parcel layers.
- Keep fill enabled automatically when continuous gradient mode is enabled.
- Use canonical feature properties for GPU polygon color buffers when runtime layer hydration is metadata-only.
- Move generic polygon GPU coloring out of `zoning_runtime_service` into a better-named service, such as `polygon_runtime_service`.
- Keep zoning category colors and continuous numeric choropleths as separate style modes in the shared polygon runtime.

## Tests

- Add a focused regression test proving hover detail and click selection resolve the same county parcel through `unified_parcels`.
- Add a test for recovery after a transient DuckDB lock or stale `last_rebuild_ok=false` state.
- Add a polygon choropleth color-buffer test that verifies multiple feature colors for CDC PLACES tract layers.
- Keep REST county parcel tests because they exercise the real app startup, GPU residency, selection, and DuckDB lookup path together.
