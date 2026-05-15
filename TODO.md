# TODO

## Highest Priority

- Make `regional_parcels.geojson.canonical.bin` the primary statewide parcel artifact.
  - Stop requiring `regional_parcels.geojson` for the normal parcel build/runtime path.
  - Keep GeoJSON as optional export/debug output only.
- Refresh downstream parcel artifacts from the completed statewide Maryland layer.
  - Warm hydration cache for `regional_parcels.geojson`.
  - Warm triangulation cache for `regional_parcels.geojson`.
  - Warm parcel render sidecar for `regional_parcels.geojson`.
  - Rebuild DuckDB from the new statewide parcel layer.
  - Record the new statewide parcel count in docs/status tooling.

## Parcel Build Pipeline

- Refactor `worldsim_regional_parcel_builder` into a two-stage pipeline.
  - Stage 1: parallel county/state source normalization into per-jurisdiction shards.
  - Stage 2: deterministic merge/finalize into canonical statewide parcel outputs.
- Remove whole-file DOM parsing from the parcel builder where practical.
  - Prefer streaming readers over full `nlohmann::json` materialization for large county inputs.
- Add explicit temp-file plus atomic-rename behavior for statewide parcel outputs.
  - Do not leave partially written `regional_parcels.geojson` or `.canonical.bin` in place after interruption.
- Add a dedicated shard/merge CLI mode.
  - Example: build shards, merge shards, export GeoJSON on demand.

## Canonical Parcel Binary

- Add a dedicated self-test for `regional_parcels.geojson.canonical.bin`.
  - Validate header/magic/version.
  - Validate stored source signature.
  - Validate feature count round-trip.
  - Validate representative geometry/property decoding.
- Add a CLI inspection tool for canonical parcel binary metadata.
  - Print source signature, feature count, and basic size/layout stats.
- Consider chunking the canonical binary.
  - Current canonical binary is dense but still monolithic.
  - Add chunk tables if runtime random access or partial rebuilds become necessary.
- Evaluate reducing canonical property payload size.
  - Current canonical binary stores full hydrated properties.
  - Consider property dictionary/string-table encoding if this becomes the primary persisted artifact.

## Runtime Hydration

- Make hydration prefer canonical parcel binary before source GeoJSON for statewide parcels as the primary source path, not just a fallback when hydration cache is missing.
- Add `/status` visibility for canonical-binary source use.
  - Example phases already added in worker code should be surfaced and verified in live status/UI.
- Add a headless validation path that compares:
  - hydrated feature count from GeoJSON
  - hydrated feature count from canonical binary
  - mismatch detection by signature and count

## Render Pipeline

- Move parcel startup residency to load from canonical parcel binary -> hydration/triangulation/render sidecar path without reparsing GeoJSON.
- Add dirty-range updates for parcel RGBA buffers.
  - Current model is long-lived geometry plus mutable color buffers.
  - Reduce full-buffer rewrites when only a subset of parcels changes color.
- Add parcel GPU residency diagnostics.
  - live feature count
  - buffer sizes
  - upload time
  - locked source signature
  - restart-required state
- Add a dedicated validation mode for GPU parcel rendering.
  - Assert fill, overlay, outline, and selected-emphasis paths all render against the retained parcel buffers.

## DuckDB / Analytics

- Rebuild `unified_parcels` from the full statewide parcel layer and report:
  - total unified parcels
  - with geometry
  - with property record
  - by jurisdiction counts
- Verify Maryland parcel detail lookups still work correctly against the larger statewide dataset.
- Keep DuckDB geometry-free for render purposes.
  - DuckDB remains parcel facts/joins/analytics only.
  - Do not regress into using DuckDB as a geometry draw source.

## Data Quality

- Validate all 24 Maryland jurisdictions are present in the rebuilt statewide parcel layer.
- Add a jurisdiction-count audit command.
  - Count parcels by `jurisdiction`.
  - Detect missing or unexpectedly tiny counties.
- Validate normalization quality across counties.
  - `source_parcel_id`
  - `account_id`
  - `blocklot`
  - `address`
  - `owner`
- Add mismatch diagnostics for parcel/property joins at statewide scale.

## Observability / Operations

- Add a progress/status surface for statewide parcel builds.
  - current source jurisdiction
  - completed jurisdictions
  - features written
  - output bytes written
- Add an operational command that rebuilds the full statewide parcel stack end to end.
  - canonical parcel binary
  - optional GeoJSON export
  - hydration cache
  - triangulation cache
  - parcel render sidecar
  - DuckDB
- Add disk-usage reporting for the parcel artifact stack.
  - GeoJSON
  - canonical binary
  - hydration cache
  - triangulation cache
  - render sidecar

## Documentation

- Update docs after the first full statewide refresh is complete with final counts and artifact sizes.
- Document the new primary artifact contract clearly:
  - canonical binary is primary
  - GeoJSON is optional/dev/export
  - render sidecar is GPU-oriented
  - DuckDB is analytics-oriented
