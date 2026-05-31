# TODO

## Current State

The statewide parcel stack now runs directly from per-jurisdiction SSOT-backed layers.

Target persisted artifacts:

- per-jurisdiction canonical layer binaries under `data/world/.../layers/*.canonical.bin`
- per-jurisdiction compiled geometry artifacts under `data/cache/geometry/`
- `data/worldsim.duckdb`

Implemented support:

- Hydration can read canonical per-layer binaries directly.
- Compiled geometry artifacts are the intended runtime geometry path.
- `--parcel-artifact-health` reports parcel artifact presence, signatures, counts, sizes, and recommended rebuild steps.
- DuckDB is treated as analytics/search/detail cache, not render geometry.
- Runtime status text identifies which hydration/triangulation artifact is being read.
- CLI/self-test coverage exists for hydration cache, triangulation cache, parcel render sidecar, canonical parcel binary, render policy, spatial index, and parcel GPU CPU-bypass behavior.

## Highest Priority

- Keep runtime-cache refreshes non-destructive by default: hydration, triangulation, and parcel render sidecar are rebuildable performance artifacts; canonical binary and DuckDB require explicit rebuild intent.
- Update docs/status output with final statewide parcel count, jurisdiction counts, and artifact sizes.

## Build IO

- Remove whole-file DOM parsing where practical.
- Prefer streaming readers/writers for large county inputs and direct per-jurisdiction outputs.
- Use temp-file plus atomic-rename behavior for every accepted output.
- Do not leave partially written `.geojson`, `.canonical.bin`, or shard files in accepted paths after interruption.
- Add dedicated CLI modes:
  - build shards
  - merge shards
  - rebuild full parcel stack

## Canonical Parcel Binary

- Keep canonical binary read paths primary for statewide parcels.
- Keep and extend canonical binary self-test coverage:
  - header/magic/version
  - source signature
  - feature count round trip
  - representative geometry decode
  - representative property decode
- Keep and extend canonical binary inspection:
  - source signature
  - feature count
  - bounds
  - size/layout stats
  - string/property payload stats
- Consider chunking the canonical binary if random access or partial rebuilds become necessary.
- Consider dictionary/string-table encoding for repeated property keys and values.
- Keep full raw county property bags out of runtime geometry caches unless a concrete runtime use requires them.

## Runtime Hydration

- Do not reintroduce any synthetic statewide parcel aggregate into the runtime cache dependency graph.
- Avoid parsing GeoJSON during normal interactive startup.
- Make `/status` clearly report canonical-binary source use and cache source use.
- Add a headless validation command that compares:
  - GeoJSON feature count, when GeoJSON exists
  - canonical binary feature count
  - hydration cache feature count
  - source signatures
  - representative geometry/property samples
- Ensure missing GeoJSON is a supported normal condition.

## Render Pipeline

- Keep statewide parcel drawing on the retained Vulkan parcel path.
- Do not regress into full CPU parcel scans when the statewide render sidecar is missing or loading.
- Move parcel startup residency toward:
  - canonical binary
  - hydration cache
  - triangulation cache
  - render sidecar
  - async GPU upload
- Add dirty-range updates for parcel color buffers.
- Avoid full color-buffer rewrites when only a subset of parcels changes state.
- Expand parcel GPU diagnostics:
  - resident feature count
  - vertex/index/color buffer sizes
  - upload time
  - visible chunk count
  - locked source signature
  - restart-required state
- Add a validation mode for retained parcel rendering:
  - fill path
  - overlay path
  - outline path
  - selected parcel emphasis
  - property-value choropleth colors

## DuckDB / Analytics

- Rebuild `unified_parcels` from the finalized statewide parcel layer after any canonical parcel refresh.
- Rebuild parcel relationship artifacts after any canonical parcel, property,
  event, or source-feature refresh:
  - `parcel_property_records`
  - `parcel_related_events`
  - `parcel_related_features`
  - `parcel_relationships`
  - `parcel_relationship_summary`
- Report:
  - total unified parcels
  - parcels with geometry
  - parcels with property record
  - parcel relationship counts by relation type
  - counts by jurisdiction
- Verify Maryland parcel detail lookups against the statewide dataset.
- Verify owner detail pages from DuckDB relationships, including owners whose
  clicked parcel was resolved by county-layer fallback rather than the in-memory
  primary parcel snapshot.
- Keep DuckDB geometry-free for render purposes.
- Keep DuckDB rebuild explicit/user-initiated, not automatic startup work.
- Add stale/missing DuckDB messaging that distinguishes analytics unavailability from render readiness.

## Data Quality

- Validate all 24 Maryland jurisdictions are present.
- Add a jurisdiction-count audit command.
- Detect missing or unexpectedly tiny county/jurisdiction contributions.
- Validate normalized parcel fields by jurisdiction:
  - `jurisdiction`
  - `source_file`
  - `source_parcel_id`
  - `account_id`
  - `blocklot`
  - `address`
  - `owner`
- Add mismatch diagnostics for parcel/property joins at statewide scale.
- Track join rates by jurisdiction and source.

## Basemaps / Dark Mode

- Keep night-lights imagery as low/mid-zoom context only.
- Use dark satellite transform for high-zoom dark-mode detail.
- Treat basemaps as typed sources:
  - raster tiles
  - raster transforms
  - vector overlays
  - night-lights context layers
  - local preprocessed raster pyramids
- Add a preprocessing path for NASA Black Marble if better night-lights context is required.
- Do not present low-resolution night-lights as parcel-scale satellite imagery.

## Observability / Operations

- Add progress/status for statewide parcel builds:
  - active source jurisdiction
  - completed jurisdictions
  - features written
  - output bytes written
  - current output artifact
- Keep disk-usage reporting current for the parcel artifact stack:
  - canonical binary
  - hydration cache
  - triangulation cache
  - render sidecar
  - DuckDB
- Extend `--parcel-artifact-health` with jurisdiction coverage and join-rate summaries.

## Documentation

- Document the primary parcel artifact contract:
  - canonical binary is the primary parcel source
  - no maintained GeoJSON layer export
  - hydration cache is CPU runtime geometry
  - triangulation cache is CPU fill geometry acceleration
  - render sidecar is GPU-oriented retained parcel geometry
  - DuckDB is analytics/search/detail only
- Document expected startup behavior when all caches are warm.
- Document expected startup behavior when each cache is missing.
- Document how to rebuild the full parcel stack safely.
- Keep `DATAFLOW.md`, `TESTS.md`, and this file aligned after each artifact-contract change.

## Deferred

- Chunked canonical binary random access.
- Partial jurisdiction rebuilds.
- Background DuckDB rebuild job.
- Multi-source basemap registry loaded from external JSON.
- Offline NASA Black Marble tile pyramid builder.
- GPU-side parcel color derivation for large choropleth changes.
