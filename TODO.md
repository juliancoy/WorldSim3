# TODO

## Current State

The statewide parcel stack has been materialized, but the canonical parcel binary is not yet the sole primary runtime artifact.

Present artifacts:

- `data/layers/regional_parcels.geojson`
- `data/layers/regional_parcels.geojson.canonical.bin`
- `data/cache/hydration/regional_parcels.geojson.bin`
- `data/cache/triangulation/regional_parcels.geojson.tri.bin`
- `data/cache/render/regional_parcels.geojson.parcel-render.bin`
- `data/worldsim.duckdb`

Implemented support:

- Hydration can read the binary hydration cache.
- `regional_parcels.geojson.canonical.bin` is now the authoritative source-signature provider for statewide parcels.
- Hydration can read the canonical parcel binary when the hydration cache is missing or stale.
- Parcel render sidecar can be warmed from binary hydration and triangulation caches.
- Hydration, triangulation, and parcel render cache warming operate under the canonical statewide parcel signature.
- `--warm-parcel-runtime-stack [regional_parcels.geojson]` warms hydration, triangulation, and parcel render caches in order without implicitly rebuilding canonical source artifacts or DuckDB.
- `--parcel-artifact-health` reports parcel artifact presence, signatures, counts, sizes, and recommended rebuild steps.
- DuckDB is treated as analytics/search/detail cache, not render geometry.
- Runtime status text identifies which hydration/triangulation artifact is being read.
- CLI/self-test coverage exists for hydration cache, triangulation cache, parcel render sidecar, canonical parcel binary, render policy, spatial index, and parcel GPU CPU-bypass behavior.

## Highest Priority

- Remove remaining operational reliance on `regional_parcels.geojson` outside explicit GeoJSON validation/export flows.
- Keep `regional_parcels.geojson` as optional export/debug interchange only.
- Add explicit source/analytics rebuild commands that can be safely composed with `--warm-parcel-runtime-stack` for full parcel-stack refreshes:
  - canonical parcel binary
  - optional GeoJSON export
  - DuckDB analytics cache
- Keep runtime-cache refreshes non-destructive by default: hydration, triangulation, and parcel render sidecar are rebuildable performance artifacts; canonical binary and DuckDB require explicit rebuild intent.
- Update docs/status output with final statewide parcel count, jurisdiction counts, and artifact sizes.

## Parcel Build Pipeline

- Refactor `worldsim_regional_parcel_builder` into explicit stages:
  - normalize county/state inputs into per-jurisdiction shards
  - merge shards deterministically into canonical statewide output
  - export GeoJSON only when requested
- Remove whole-file DOM parsing where practical.
- Prefer streaming readers/writers for large county inputs and statewide outputs.
- Use temp-file plus atomic-rename behavior for every statewide output.
- Do not leave partially written `.geojson`, `.canonical.bin`, or shard files in accepted paths after interruption.
- Add dedicated CLI modes:
  - build shards
  - merge shards
  - export GeoJSON from canonical binary
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

- Prefer canonical parcel binary before source GeoJSON for `regional_parcels.geojson` cache rebuilds.
- Avoid parsing 7 GB GeoJSON during normal interactive startup.
- Make `/status` clearly report canonical-binary source use, cache source use, and fallback source use.
- Add a headless validation command that compares:
  - GeoJSON feature count, when GeoJSON exists
  - canonical binary feature count
  - hydration cache feature count
  - source signatures
  - representative geometry/property samples
- Ensure missing GeoJSON is a supported normal condition when canonical binary and required caches exist.

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
- Report:
  - total unified parcels
  - parcels with geometry
  - parcels with property record
  - counts by jurisdiction
- Verify Maryland parcel detail lookups against the statewide dataset.
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
  - optional GeoJSON
  - canonical binary
  - hydration cache
  - triangulation cache
  - render sidecar
  - DuckDB
- Extend `--parcel-artifact-health` with jurisdiction coverage and join-rate summaries.

## Documentation

- Document the primary parcel artifact contract:
  - canonical binary is the primary parcel source
  - GeoJSON is optional export/debug interchange
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
