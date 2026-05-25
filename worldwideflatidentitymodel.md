# Worldwide Flat Identity Model

## Goal

Define an identity system that can represent arbitrary partial geospatial datasets from any geography level while preserving:

- stable lookup from rendered feature to analytic record
- support for incomplete and overlapping source coverage
- coexistence of parcel, block, tract, municipality, county, state, national, and supra-national data
- append-only ingestion from heterogeneous sources
- deterministic deduplication and reconciliation without requiring full global coverage

This model is intentionally flat at the identity layer. Hierarchies are attached as relations, not encoded into the primary key.

## Design Principles

1. Identity must not depend on layer index, render order, file path, or local feature index.
2. Identity must survive reimports, geometry recompilation, cache rebuilds, and UI/runtime refactors.
3. Partial datasets are normal. Missing parents, children, or neighboring jurisdictions must not invalidate identity.
4. Geographic hierarchy is metadata, not identity.
5. Source truth and canonical truth must both be preserved.
6. Reconciliation must be reversible and auditable.
7. Legacy identity paths must be removed, not retained as permanent fallback.

## Core Entities

### 1. `entity`

The universal identity table.

Each row is one real-world or source-defined object.

Suggested fields:

- `entity_id UUID/ULID`
- `entity_kind TEXT`
- `identity_class TEXT`
- `canonical_name TEXT NULL`
- `status TEXT`
- `created_at`
- `updated_at`

`entity_kind` examples:

- `parcel`
- `property_record`
- `building`
- `address`
- `road_segment`
- `block`
- `tract`
- `municipality`
- `county`
- `state_region`
- `nation_state`
- `custom_area`

`identity_class` examples:

- `canonical`
- `provisional`
- `source_native`
- `derived`

Notes:

- `entity_id` is the only primary identity used across runtime, DuckDB, caches, APIs, and UI.
- Never encode geography hierarchy into `entity_id`.

### 2. `source_dataset`

Describes the originating dataset.

Suggested fields:

- `source_dataset_id UUID/ULID`
- `source_system TEXT`
- `dataset_name TEXT`
- `dataset_version TEXT NULL`
- `dataset_date TEXT NULL`
- `jurisdiction_label TEXT NULL`
- `geometry_level TEXT NULL`
- `license TEXT NULL`
- `provenance_url TEXT NULL`
- `ingest_signature TEXT`

### 3. `source_feature`

Represents one record as it existed in a source dataset.

Suggested fields:

- `source_feature_id UUID/ULID`
- `source_dataset_id`
- `source_primary_key TEXT`
- `source_path TEXT NULL`
- `source_row_number BIGINT NULL`
- `source_feature_hash TEXT`
- `entity_id`
- `geometry_entity_id UUID/ULID NULL`
- `valid_from`
- `valid_to`

Uniqueness:

- unique on `(source_dataset_id, source_primary_key)` when the source key exists
- otherwise unique on `(source_dataset_id, source_feature_hash)`

Notes:

- This is the audit bridge from raw source to canonical entity.
- Multiple `source_feature` rows may map to the same `entity_id`.

### 4. `entity_geometry`

Stores geometry identity separately from business identity.

Suggested fields:

- `geometry_entity_id UUID/ULID`
- `entity_id`
- `geometry_kind TEXT`
- `geometry_hash TEXT`
- `coverage_scope TEXT`
- `centroid_lon DOUBLE`
- `centroid_lat DOUBLE`
- `bbox_min_lon DOUBLE`
- `bbox_min_lat DOUBLE`
- `bbox_max_lon DOUBLE`
- `bbox_max_lat DOUBLE`
- `source_dataset_id`
- `geometry_signature TEXT`

Notes:

- One business entity can have multiple geometries across time, resolutions, or sources.
- Picking can target `geometry_entity_id` or `entity_id`, depending on artifact design.

### 5. `entity_attribute`

Append-only attribute facts.

Suggested fields:

- `entity_attribute_id UUID/ULID`
- `entity_id`
- `attribute_namespace TEXT`
- `attribute_key TEXT`
- `value_text TEXT NULL`
- `value_num DOUBLE NULL`
- `value_json JSON NULL`
- `unit TEXT NULL`
- `source_feature_id`
- `confidence DOUBLE`
- `observed_at`

Notes:

- Do not force one giant denormalized row as the only truth model.
- Materialized wide tables can be derived for performance.

### 6. `entity_relation`

Represents topology, hierarchy, and reconciliation.

Suggested fields:

- `relation_id UUID/ULID`
- `subject_entity_id`
- `predicate TEXT`
- `object_entity_id`
- `source_feature_id NULL`
- `confidence DOUBLE`
- `relation_scope TEXT`
- `created_at`

Important predicates:

- `within`
- `contains`
- `overlaps`
- `adjacent_to`
- `same_as`
- `derived_from`
- `split_from`
- `merged_from`
- `address_of`
- `building_on`
- `record_for`

This is where geography hierarchy lives.

## Identity Rules

### Canonical ID Rule

Every object gets a globally unique `entity_id` at first ingest.

If later reconciliation proves two entities are the same:

- do not destroy provenance
- add a `same_as` relation or merge map
- choose one surviving canonical `entity_id`
- mark the other as superseded

### Source-Native ID Rule

Never discard the source’s own identifier.

Examples:

- parcel account number
- assessor parcel number
- blocklot
- cadastral ID
- title ID
- municipal feature ID

Store it in `source_feature.source_primary_key` and optionally mirror normalized forms in `entity_attribute`.

### Geometry Rule

Geometry identity is not business identity.

This matters because:

- one parcel can have revised geometry over time
- one source can provide centroid-only partial data
- one entity can have multiple valid geometries at different scales

## Support For Partial Datasets

The model assumes four common partial-data cases:

1. Attribute-only parcel records with no geometry
2. Geometry-only parcel polygons with weak attributes
3. Jurisdiction-wide coverage for some places and sparse coverage elsewhere
4. Mixed levels, such as county parcels plus tract-level demographics plus city zoning

How partial support works:

- `entity` exists even without geometry
- `entity_geometry` exists even without rich attributes
- hierarchy does not require full ancestry at ingest time
- missing relations are nullable, not fatal
- reconciliation can improve the graph incrementally

## Flat Pick-To-Data Contract

Rendered artifacts should carry a stable identity payload:

- preferred: `entity_id`
- alternate stable geometry key when entity semantics do not exist: `geometry_entity_id`
- source provenance for traceability: `(source_dataset_id, source_primary_key)`

Never use:

- layer-local feature index as the long-term contract
- runtime vector position
- cache-local record offset

### Recommended Render Artifact Fields

For every renderable feature record:

- `entity_id`
- `geometry_entity_id`
- `source_dataset_id`
- `source_primary_key_hash`
- `feature_idx_local` for debugging only

On click:

1. GPU pick returns `entity_id` or `geometry_entity_id`
2. runtime resolves business entity
3. DuckDB queries by `entity_id`
4. UI joins any related hierarchy or source rows as needed

## Geography Hierarchy Model

Do not encode hierarchy inside IDs.

Instead use `entity_relation`:

- parcel `within` block
- block `within` tract
- tract `within` county
- county `within` state_region
- state_region `within` nation_state

This allows:

- incomplete hierarchy
- multiple competing hierarchies
- disputed borders
- nonstandard administrative systems
- custom operational geographies

## Reconciliation Strategy

### Levels of Match

1. `exact_source_key`
2. `normalized_external_id`
3. `geometry_and_address`
4. `geometry_overlap_and_name`
5. `human_review_required`

### Reconciliation Outputs

Each reconciliation pass may:

- attach new `source_feature` rows to an existing `entity_id`
- create `same_as` relations
- create `record_for` relations between property and parcel entities
- create supersession records for retired IDs

### Non-Goals

Do not force global deduplication at ingest time.

Professional practice is:

- ingest first
- preserve provenance
- reconcile incrementally
- keep uncertainty explicit

## DuckDB Physical Model

Recommended primary analytic tables:

### `entities`

- `entity_id`
- `entity_kind`
- `identity_class`
- `status`

### `source_features`

- `source_feature_id`
- `source_dataset_id`
- `source_primary_key`
- `entity_id`
- `geometry_entity_id`

### `entity_geometries`

- `geometry_entity_id`
- `entity_id`
- `geometry_kind`
- `geometry_hash`
- bbox columns
- centroid columns

### `entity_attributes_wide_parcel`

Materialized convenience table, not source truth.

- `entity_id`
- parcel/business fields commonly used in UI and filters

### `entity_relations`

- `subject_entity_id`
- `predicate`
- `object_entity_id`
- `confidence`

### `render_pick_index`

Optional fast lookup table:

- `geometry_entity_id`
- `entity_id`
- `active_artifact_signature`

## Migration From Layer-Indexed Identity

If current runtime uses `(layer_idx, feature_idx)`, replace it directly:

1. Add `entity_id` to canonical feature rows and compiled geometry artifacts.
2. Replace GPU pick result payloads with `entity_id`.
3. Replace DuckDB detail queries with `entity_id`.
4. Delete the old layer-indexed lookup path.

There is no supported fallback identity contract.

## Required Code Changes

This section maps the target identity model to the current repository structure.

### 1. Extend the core feature model

Current problem:

- `LayerDef::FeatureRecord` in `types.h` carries geometry and triangles, but no
  stable per-feature identity fields.

Required change:

- add `entity_id`
- add `geometry_entity_id`
- add `source_feature_id`
- add `source_primary_key` or normalized stable `feature_id`

Files:

- `types.h`
- any builders that populate `LayerDef::FeatureRecord`

Result:

- every in-memory feature can carry stable identity without depending on layer
  position.

### 2. Persist identity in canonical binaries

Current problem:

- canonical layer binaries currently preserve feature content but not the full
  identity contract required by the new model.

Required change:

- version the canonical binary format
- write/read `entity_id`
- write/read `geometry_entity_id`
- write/read `source_feature_id`
- write/read stable source-level feature ID

Files:

- `cache_io.cpp`
- `cache_io.h`
- any canonical validation/selftest paths in `worldsim_cli.cpp`

Result:

- identity survives import, rebuild, cache invalidation, and restart.

### 3. Populate identity at import/build time

Current problem:

- imported features are derived from source geometry and properties, but global
  identity is not minted or attached during build.

Required change:

- mint stable IDs during source import
- preserve source-native keys
- write source provenance rows or sidecar mappings used to build DuckDB tables
- make identity assignment deterministic for repeated imports of unchanged
  source records

Files:

- `layer_import.cpp`
- `layer_geometry.cpp`
- source-specific import helpers

Result:

- identity originates from the ingestion pipeline rather than from runtime state.

### 4. Persist identity in compiled geometry artifacts

Current problem:

- geometry artifacts preserve feature-local references, but current repo docs and
  code still permit layer-scoped feature identity as the main join path.

Required change:

- add `entity_id` and `geometry_entity_id` to geometry artifact feature records
- keep dense integer indices only as internal acceleration fields
- ensure GPU-ready feature records can map directly back to stable identity

Files:

- `cache_io.h`
- `cache_io.cpp`
- `polygon_artifact_runtime_service.cpp`
- `feature_overlay_gpu_service.cpp`
- any point/polyline artifact loaders

Result:

- geometry artifacts become stable pick/query bridges rather than layer-local
  caches.

### 5. Change GPU pick output contract

Current problem:

- GPU pick currently returns `feature_idx` for parcel/zoning paths.

Required change:

- return `entity_id` or `geometry_entity_id` from pick buffers
- remove `feature_idx` as a lookup contract
- change pick result structs and downstream selection code to use stable IDs

Files:

- `gpu_pick_service.cpp`
- `worldsim_app.cpp`
- map interaction and selection modules
- any pick-result structs in headers

Result:

- rendered surface interaction no longer depends on layer-local vector position.

### 6. Replace parcel-layer local selection state

Current problem:

- parcel selection in the runtime is anchored on `selected_parcel_idx`,
  `selected_parcel_indices`, and stable lookup through parcel feature index.

Required change:

- make selected state primarily `entity_id`-based
- delete old feature-index selection fields
- store selected geometry/entity IDs in UI state persistence

Files:

- `app_main_loop.cpp`
- `selection.cpp`
- `selection.h`
- `parcel_info_tab.cpp`
- `owner_info.cpp`
- `layer_state_io.cpp`
- any UI persistence JSON writers/readers

Result:

- selection survives source reshaping, layer reorder, and artifact rebuilds.

### 7. Rebuild `unified_parcels` on stable entity identity

Current problem:

- `UnifiedParcelRecord` and DuckDB `unified_parcels` currently revolve around
  `parcel_layer_idx` and `parcel_feature_idx`.

Required change:

- add `parcel_entity_id`
- add `parcel_geometry_entity_id`
- add `property_entity_id` where applicable
- remove `parcel_layer_idx` and `parcel_feature_idx` as logical primary keys
- move logical joins to `entity_id`

Files:

- `parcel_unified.h`
- `parcel_unified.cpp`
- `duckdb_analytics.cpp`
- `duckdb_analytics.h`
- parcel detail/query helpers

Result:

- parcel business logic is keyed by canonical parcel identity rather than by
  current layer position.

### 8. Change DuckDB schemas and queries

Current problem:

- current analytical lookups still query by `(parcel_layer_idx,
  parcel_feature_idx)`.

Required change:

- add `entity_id` columns to `layer_features`
- add `entity_id`/`geometry_entity_id` to parcel-facing tables
- add `source_feature_id` and `source_dataset_id` where provenance matters
- index by `entity_id`
- update detail, search, filter, and query SQL to resolve through stable
  identity

Files:

- `duckdb_analytics.cpp`
- `duckdb_analytics.h`
- SQL/query UI and helper modules

Result:

- UI and query paths can resolve details without depending on current runtime
  layer coordinates.

### 9. Update render-time metadata consumers

Current problem:

- several runtime helpers infer parcel/zoning feature resolution through
  `ParcelRenderFeatureRecord.feature_idx` or layer-local artifact references.

Required change:

- add stable IDs to render metadata records
- make overlay, inspection, metrics, and hover helpers resolve through those IDs
- preserve dense offsets only for GPU buffer traversal

Files:

- `map_render_selection.cpp`
- `map_inspection.cpp`
- `parcel_metrics.cpp`
- `parcel_runtime_service.cpp`
- `zoning_runtime_service.cpp`

Result:

- render metadata becomes identity-aware across all geometry families.

### 10. Introduce source provenance tables

Current problem:

- the repository tracks source signatures well, but not yet a full durable
  source-feature provenance graph.

Required change:

- add `source_dataset`
- add `source_feature`
- add `entity_relation`
- add `entity_geometry`
- materialize them in DuckDB or another durable store used by builders

Files:

- `duckdb_analytics.cpp`
- builder/import flows
- documentation and validation tooling

Result:

- partial data, overlapping jurisdictions, and reconciliation become explicit
  rather than implicit.

### 11. Update query/filter replay contracts

Current problem:

- repeatable queries and map overlays still expose layer-local feature
  coordinates in examples and current replay contracts.

Required change:

- include `entity_id` in query result payloads when available
- migrate replayable filter/query formats to support stable identity references
- delete old `layer_idx/feature_idx` identity outputs from the contract

Files:

- `DATAFLOW.md`
- `worldsim_cli.cpp`
- query UI modules
- status API / local API outputs

Result:

- exported selections and query results remain stable across rebuilds.

## Legacy Removal Requirement

The repository should not preserve the following as supported fallback identity
contracts after migration:

- `(layer_idx, feature_idx)`
- `(parcel_layer_idx, parcel_feature_idx)`
- layer-order-dependent selection state
- render-artifact-local record offsets as external identity
- cache-path or file-path-derived feature identity

Legacy code should be deleted, not hidden behind runtime switches.

That includes:

- old pick-result structs that expose feature index as the primary key
- compatibility SQL queries keyed by parcel/layer feature index
- selection persistence keyed by feature index
- helper functions whose purpose is to translate stable identity back into
  layer-local identity as an external contract
- migration-only compatibility helpers
- temporary dual-key payloads

If fallback identity code is found, remove it.

### 12. Add migration and verification tooling

Required change:

- add CLI to inspect identity coverage in canonical binaries
- add CLI to inspect identity coverage in geometry artifacts
- add CLI to validate DuckDB identity joins
- add assertions that picked IDs resolve to exactly one entity row

Files:

- `worldsim_cli.cpp`
- test registrations in `CMakeLists.txt`
- any selftest docs

Result:

- migration can be validated incrementally rather than by visual inspection.

## Recommended Implementation Order

The safest order is:

1. Add identity fields to `FeatureRecord`.
2. Populate them during import.
3. Persist them in canonical binaries.
4. Persist them in geometry artifacts.
5. Add them to DuckDB tables.
6. Switch GPU pick and selection to stable identity.
7. Switch parcel/zoning detail queries to `entity_id`.
8. Delete legacy layer-local identity assumptions immediately after verification.

## Current Repository Areas That Will Break If Changed Out Of Order

- `gpu_pick_service.cpp` and parcel/zoning selection logic
- `parcel_unified.cpp` assumptions about direct feature-index alignment
- DuckDB parcel detail queries keyed by layer/feature index
- map inspection and overlay code that expects feature index equality between
  rendered artifacts and `LayerDef::features`

Do not flip pick output to `entity_id` before the DuckDB and runtime selection
layers can resolve `entity_id` directly. Once they can, remove the old path.

## Rules For Arbitrary Geography Levels

This model works across arbitrary levels because:

- identity is flat
- hierarchy is relational
- source provenance is explicit
- geometry is separable
- canonicalization is optional and incremental

Examples that can coexist:

- parcel entity in one county
- municipality boundary entity
- neighborhood polygon entity
- national cadastral record
- census tract entity
- custom NGO survey polygon

All are peers in `entity`.

## Operational Constraints

To keep this practical:

- use compact binary identity columns in render artifacts
- index DuckDB on `entity_id`, `geometry_entity_id`, `source_dataset_id`
- materialize parcel-focused wide tables for UI speed
- keep reconciliation jobs offline or incremental
- version every compiled artifact by source signature plus schema version

## Minimum Viable Implementation

If implementing in stages, the minimum acceptable target is:

1. Introduce `entity_id` for every parcel and zoning feature.
2. Store `entity_id` in canonical binaries.
3. Store `entity_id` in polygon GPU artifacts.
4. Return `entity_id` from GPU picking.
5. Add DuckDB tables keyed by `entity_id`.
6. Keep source-layer identity only as compatibility metadata.

## Summary

The correct identity system for arbitrary partial worldwide geodata is:

- one flat global `entity_id`
- explicit source-feature provenance
- separate geometry identity
- relational hierarchy
- incremental reconciliation
- render artifacts keyed by stable entity identity

That is the model that scales across partial coverage, mixed geography levels, and evolving source systems without coupling identity to runtime layout.
