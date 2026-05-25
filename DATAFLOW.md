# WorldSim3 Data Flow

This document describes how layer data should move from SSOT source payloads into canonical layer binaries, compiled geometry artifacts, DuckDB attribute tables, derived records, and runtime GPU state.

## Source Identity

Every runtime-readable layer is identified by the embedded `source_signature` in its `.canonical.bin` artifact from `cache_io.cpp`.

This signature is the invalidation key shared by compiled geometry artifacts, derived caches, and DuckDB analytics.

`source_signature` is an artifact-generation identity, not a per-feature or
per-entity business identity. The target shape for feature/entity identity is
described in `worldwideflatidentitymodel.md`: flat global entity IDs, separate
geometry IDs, explicit source provenance rows, and hierarchy expressed as
relations.

Current render and filter code still derives dense runtime coordinates such as
`(layer_idx, feature_idx)` from stable entity IDs when it needs direct geometry
access. Those coordinates are implementation detail only; query and selection
contracts should expose stable entity IDs instead.

## Disk-Persisted Artifact Overview

WorldSim3 should converge on three broad classes of disk artifacts:

- Authoritative source payloads and metadata under `sources/` and `data/inbox/`.
- Canonical layer binaries under `data/world/.../layers/*.canonical.bin`.
- Compiled geometry artifacts under `data/cache/geometry/`.
- Attribute/query artifacts under `data/worldsim.duckdb` and analytical outputs under `data/analytics/`.
- Runtime/user state JSON files under `data/`.

The startup rule should be intentionally simple:

- Geometry startup reads compiled geometry artifacts that are already shaped for runtime/GPU use.
- Attribute startup opens `data/worldsim.duckdb` and reads text, numeric, join, filter, detail, and query-oriented feature data from DuckDB tables.
- Canonical layer binaries are build/import artifacts and rebuild inputs. They are not the normal interactive startup source for attributes, joins, filters, or render geometry.
- Runtime state should orchestrate GPU artifact residency and DuckDB query readiness, not retain CPU geometry/property bags as an interaction fallback.

## Single High-Grade Pipeline Rule

Best practice is to keep exactly one canonical geometry pipeline and one canonical attribute pipeline per feature family.

For WorldSim3 that means:

- Compiled geometry artifacts are the canonical persisted representation used for rendering and GPU picking.
- DuckDB tables are the canonical persisted representation used for searchable, filterable, inspectable feature attributes.
- Derived caches are allowed only when they accelerate a bounded workflow and do not redefine canonical truth.
- UI features should not require a second hidden pipeline for the same fact set.

The anti-pattern is a split-brain feature:

- geometry lives in one ad hoc cache shape for one layer family
- hit testing depends on full CPU geometry for another
- text/detail data is duplicated across geometry caches, runtime-only joins, and DuckDB

That is lower-grade than a single pipeline because it creates contradictory readiness states, duplicated storage formats, and repeated transform logic.

The preferred pattern is:

```text
source payloads / SSOT
  -> canonical layer binaries
  -> geometry compiler
     -> direct-uploadable geometry artifact
  -> attribute ingester
     -> DuckDB canonical feature tables
  -> optional DuckDB-derived domain tables/caches
  -> UI-visible feature and query workflows
```

The key simplification is:

- geometry artifacts should not carry full property bags
- DuckDB should not be asked to provide render geometry
- runtime should stop treating full CPU geometry objects or canonical property bags as the primary durable interface between persisted artifacts and interaction

## Explicit Boundaries

WorldSim3 should keep hard boundaries between source artifacts, canonical domain artifacts, runtime state, and analytics materializations.

### 1. Source Payload Boundary

Files under `sources/` and source-adjacent payloads under `data/inbox/` are the persisted input boundary.

They may:

- preserve upstream geometry and attribute payloads
- normalize upstream naming and format differences
- act as stable, inspectable inputs to later build steps

They must not:

- depend on DuckDB being present
- depend on runtime-only in-memory joins
- silently change meaning based on whether the app was previously opened

Upstream GeoJSON is allowed only at the source-payload boundary, such as a direct SSOT download preserved under `sources/` or `data/inbox/`.

Files under `data/world/.../layers/*.geojson` are deprecated and should not be produced, regenerated as an intermediate build step, or maintained as a runtime-facing artifact. If any remain from older runs, they are stale legacy artifacts and must not be required for startup, hydration, signature resolution, or normal layer availability checks.

Layer identifiers in manifests and code may still retain `.geojson` suffixes as stable logical IDs. That suffix does not imply that a stored layer GeoJSON file is an accepted runtime artifact.

### 2. Canonical Domain Artifact Boundary

Canonical domain artifacts are persisted, rebuildable outputs that define the authoritative geometry and attribute facts for a workflow.

For SSOT geometry inputs, each geometry file downloaded directly from the upstream source should correspond to exactly one canonical Vulkan binary containing the same geometry data in a representation that can be uploaded directly to Vulkan.

For all feature families, this should mean:

- one compiled geometry artifact family per geometry class
- one canonical DuckDB table family for attributes and query state
- optional explicit derived artifacts when a workflow genuinely needs denormalized domain facts

These artifacts may:

- be produced by explicit builders
- join multiple persisted sources deterministically
- carry normalized fields and stable feature identifiers

They must not:

- require a live runtime session to exist
- exist only as a temporary runtime merge that disappears after shutdown
- require reparsing raw GeoJSON in order to become renderable every time the app starts
- require interactive startup to rebuild feature property bags from canonical binaries when DuckDB already contains the needed attribute columns

If a domain join is important enough for search, screening, filtering, inspection, styling, or choropleth classification, it belongs in DuckDB as a canonical or derived table. A separate binary sidecar may exist only as a DuckDB-derived acceleration artifact with the same source/build signature; it must not become a competing source of truth.

### 3. Runtime Geometry Boundary

Runtime orchestration owns session memory, not long-term truth.

Runtime layer records may:

- load compiled geometry artifacts and resident GPU buffers
- support culling, upload scheduling, and GPU picking
- hold compact session views of DuckDB-backed attributes needed for immediate interaction

They must not:

- become the only place where important attribute detail exists
- redefine the canonical meaning of a persisted artifact
- force analytical workflows to depend on frame-loop timing or CPU geometry residency
- rebuild large text/numeric/property columns from canonical binaries during normal startup when those columns are already persisted in DuckDB

Runtime-only joins are acceptable only as short-lived accelerators over DuckDB-backed columns or explicit derived artifacts. They are not acceptable as the sole durable representation of domain facts, and they should not be rebuilt by scanning canonical feature property bags on the startup critical path.

### 4. Derived Runtime Boundary

Derived runtime caches are allowed to compute narrowly scoped facts from artifact-backed layers.

Examples:

- vacancy/tax rollups
- parcel-event summaries
- harmonized in-session lookup indexes

They may:

- be invalidated by propagated source signatures
- be recomputed cheaply or incrementally
- feed UI overlays and session-local workflows

They must not:

- become a hidden second canonical source system
- carry facts that need durable auditability unless those facts are also persisted upstream as canonical artifacts
- become mandatory startup work when their source layers are absent; optional derived joins should fail quiet and remain rebuildable on demand

### 5. DuckDB Boundary

DuckDB is the attribute, semantic, and analytics materialization boundary.

DuckDB may:

- be the universal durable store for feature attributes, normalized fields, and query-oriented denormalizations
- accelerate startup semantic hydration, search, filtering, reporting, choropleths, detail panels, and ad hoc analysis
- store derived tables such as `unified_parcels` when those joins are query-facing
- store generic repository/source metadata keyed by provenance rather than region-specific one-off tables

DuckDB must not:

- be required to provide map geometry
- be treated as the render-geometry source
- be bypassed by large parallel property bags persisted inside geometry caches
- be bypassed by normal startup paths that rebuild semantic columns from canonical binary property bags
- redefine canonical truth through DB-only transforms that cannot be rebuilt from persisted inputs
- materialize geography-named special tables such as one region per table; geography belongs in rows and provenance columns, not in schema names

The correct flow is:

```text
persisted sources
  -> compiled geometry artifacts
  -> canonical DuckDB feature tables
  -> runtime geometry upload / query workflows
  -> optional derived exports and overlays
```

Not:

```text
persisted sources
  -> source-shaped cache
  -> per-layer special-case render cache
  -> separate attribute copies in runtime memory and DuckDB
  -> implied canonical truth
```

### 6. BEPS/Screening Boundary

Regulatory screening should not stop at the parcel boundary when the regulated entity is a building.

For Maryland BEPS-style work:

- parcel geometry is a spatial anchor
- property/assessment data is a screening attribute source
- building coverage logic should live in an explicit building or covered-property pipeline

Parcel-level artifacts may support first-pass screening, but final BEPS candidate logic should not be treated as fully solved by parcel joins alone.

## Repeatable Filter Boundary

Repeatable filters should be treated as explicit, versioned analytical definitions over canonical fields.

They are not the same thing as transient UI toggles in `MapFilterState`.

Recommended control surface:

- transient interactive map controls stay under `/controls/filter`
- repeatable persisted filter definitions live under `/api/filters` and on disk under `data/filters/*.json`

### What A Repeatable Filter Is

A repeatable filter is:

- a named filter definition with a stable ID
- scoped to a single entity type such as parcel, parcel-property, building candidate, event, or tract
- expressed either as normalized declarative conditions or as an explicit SQL definition over the canonical analytical entity table
- versioned so the same filter can be rerun later and produce auditable results

Examples:

- `md_beps_first_pass_v1`
- `vacant_tax_lien_priority_v2`
- `owner_concentration_screen_v1`

### What A Repeatable Filter Is Not

A repeatable filter is not:

- an unsaved slider or text box in the UI
- a renderer-local predicate
- a SQL query that only exists in a one-off session tab
- a condition written against raw source-specific field names like `STRUCTAREA` in one county and `STRCT_SQFT` in another

A SQL-backed repeatable filter is acceptable only once it has been promoted into the persisted repeatable-filter registry. The anti-pattern is ephemeral session SQL with no stable saved definition.

### Required Inputs

Condition-based repeatable filters should run only on canonical normalized fields.

For example:

- `jurisdiction`
- `state_region`
- `use_class_normalized`
- `structure_area_sq_ft`
- `year_built`
- `owner`
- `vacant_notice_count`

They should not depend directly on upstream source field names unless those are first normalized into canonical fields.

SQL-backed repeatable filters should target canonical analytical entity tables such as `unified_parcels`, not raw source-specific per-county structures.

### Definition Format

The filter definition should be persisted and versioned.

Two execution modes are allowed:

- declarative conditions over normalized fields
- explicit SQL over the canonical analytical entity table

Recommended structure:

```json
{
  "id": "md_beps_first_pass_v1",
  "entity": "parcel_property",
  "version": 1,
  "execution": {
    "mode": "conditions"
  },
  "conditions": [
    {"field": "state_region", "op": "=", "value": "md"},
    {"field": "structure_area_sq_ft", "op": ">=", "value": 35000},
    {"field": "use_class_normalized", "op": "in", "value": ["commercial", "multifamily"]}
  ],
  "provenance": {
    "square_footage_field": "structure_area_sq_ft",
    "square_footage_note": "assessment structure area used as first-pass screening proxy"
  }
}
```

SQL-backed structure:

```json
{
  "id": "owner_followup_v1",
  "entity": "parcel_property",
  "version": 1,
  "execution": {
    "mode": "sql",
    "source": "sql_tab"
  },
  "sql": "SELECT parcel_layer_idx AS layer_idx, parcel_entity_id AS entity_id, blocklot, owner, address FROM unified_parcels WHERE owner ILIKE '%llc%'",
  "presentation": {
    "name": "Owner Followup",
    "color": {"r": 1.0, "g": 0.48, "b": 0.08, "a": 1.0}
  },
  "replay_context": {
    "snapshot": {
      "filter_enabled": true,
      "center_lon": -76.6122,
      "center_lat": 39.2904,
      "zoom": 12.0,
      "map_title_text": "Owner Followup"
    }
  }
}
```

Current note:

- this shape reflects the repository's present parcel-selection contract
- future repeatable-query and map-selection outputs should be able to expose a
  stable entity identity in addition to layer-local feature coordinates
- moving queries to global entity identity is an architectural migration, not a
  reason to break existing replayable query behavior in place

The same definition should be executable by:

- runtime derived workflows
- DuckDB analytical queries
- offline export/report tooling

### Persistence Rule

If a filter matters operationally, its definition should be saved independently from UI state.

UI state may persist:

- current text filters
- map layer visibility
- temporary owner selections

Repeatable filter persistence should instead save:

- filter ID
- entity type
- version
- execution mode
- normalized field predicates or canonical SQL
- provenance notes when applicable
- optional presentation metadata
- optional replay metadata for restoring map/session context around the durable filter definition

### Result Materialization Rule

High-value repeatable filters should be materialized as derived outputs.

Examples:

- DuckDB tables or views
- exportable CSV/Parquet result sets
- saved filter result artifacts under a dedicated analytics output path

The filter definition is the contract; the result set is a rebuildable materialization of that contract.

Replay metadata is supplemental context around the contract. It may restore view state, title settings, current UI filter values, selected owners, and selected parcel blocklots, but it should not be confused with the logical filter definition itself.

### Relationship To `MapFilterState`

`MapFilterState` remains the single source of truth for UI-created interactive filters.

Left-panel geography browsing is not part of `MapFilterState`. It is separate browse-only UI state that narrows the layer catalog but does not participate in runtime rendering or query evaluation.

That state is appropriate for:

- live map exploration
- temporary narrowing of the viewport
- ad hoc owner/address/date searches

It is not sufficient by itself for:

- auditability
- reproducible screening
- cross-session comparability
- regulated workflow definitions

### Recommended Execution Order

The preferred repeatable-filter flow is:

```text
canonical domain artifacts
  -> normalized analytical entity table
  -> versioned filter definition
  -> repeatable result materialization
  -> optional UI overlay / optional DuckDB acceleration
```

This keeps repeatable filters as data products instead of accidental side effects of the current UI state.

The main artifacts are:

| Path | Kind | Producer | Consumer | Notes |
| --- | --- | --- | --- | --- |
| `data/world/.../layers/*.canonical.bin` | Canonical runtime layer binary | Explicit builders/importers | Hydration workers, layer registry, compiled geometry builders, runtime detail/query glue | Required runtime-readable layer artifact. Embedded `source_signature` replaces GeoJSON file metadata as the normal invalidation key. |
| `data/world/.../layers/*.geojson` | Legacy removed artifact class | None | None | Deprecated path. Upstream GeoJSON may exist under `sources/` or `data/inbox/`, but layer GeoJSON in this runtime-facing location must not be produced by current builders, regenerated as an intermediate artifact, or required for startup, hydration, signature resolution, or normal layer availability checks. |
| `data/world/.../layers/*.geojson.part` | In-progress legacy export/download write | Export or download tooling | Download/export finalization only | Temporary artifact; not a valid runtime layer source. |
| `sources/world/.../**/*` excluding tracked manifests | Raw imported/downloaded upstream payloads | Dataset download/import tools | Builders and audit/debug workflows | Preserves upstream ZIP/CSV/XLSX/PDF payloads used to generate canonical binaries or document the upstream source. These files are local-only working artifacts and should normally be ignored by git. |
| `data/inbox/**` | Manual drop-zone inputs | User or external process | Builder scripts/tools | Used for datasets that are copied in manually, such as HUD PIT files. |
| `data/cache/geometry/<layer-file>.point.bin` | Compiled point geometry artifact | Geometry compiler/builders | Vulkan upload path, GPU picking path | Direct-uploadable point geometry artifact with stable feature refs and picking metadata. |
| `data/cache/geometry/<layer-file>.polyline.bin` | Compiled polyline geometry artifact | Geometry compiler/builders | Vulkan upload path, GPU picking path | Direct-uploadable polyline geometry artifact with stable feature refs and picking metadata. |
| `data/cache/geometry/<layer-file>.polygon.bin` | Compiled polygon geometry artifact | Geometry compiler/builders | Vulkan upload path, GPU picking path | Direct-uploadable polygon geometry artifact with stable feature refs, fill/line draw data, and picking metadata. |
| `data/cache/geometry/*.tmp.*` | In-progress geometry artifact write | Geometry compiler | None after successful rename | Temporary artifact. A completed write atomically renames into place. |
| `data/cache/aggregate/<hex-key>.raster.bin` | Heatmap aggregate raster cache | Heatmap runtime | Heatmap runtime | Rebuildable raster cache keyed by heatmap/view/filter settings. Runtime texture objects are separate in-memory state. |
| `data/cache/derived/parcel_vacancy_status.json` | Derived parcel status cache | Derived cache refresh | Derived cache refresh and parcel styling | Stores derived parcel vacancy status records. Invalidated by propagated source signatures/generations. |
| `data/filters/*.json` | Persisted repeatable filter definitions | REST filter registry and future analytics tooling | Repeatable filter apply workflows, audit/export tooling | Versioned declarative filter specs over canonical normalized fields. Distinct from `MapFilterState` and renderer-local UI state. |
| `data/cache/screenshots/*` | User screenshots | Screenshot capture path | User/debug workflows | Output artifacts, not inputs to the layer pipeline. |
| `data/worldsim.duckdb` | Canonical attribute and query store | Explicit DuckDB analytics rebuild / future ingest pipeline | Startup semantic hydration, query/search/right-panel analytics, repeatable filters, detail panels, choropleth/filter column extraction, derived analytics | Durable home for normalized feature attributes and query-oriented denormalizations. It is not a render-geometry cache and should not be asked to become one. |
| `data/cache/derived/*.bin` | Optional DuckDB-derived column acceleration cache | DuckDB-backed derived cache builder | Startup semantic column mapping, renderer color/filter buffer construction | Optional performance sidecar for hot columns. Must carry DuckDB/source signatures and be rebuildable from `data/worldsim.duckdb`; it is not an independent source of truth. |
| `data/analytics/*` | Offline analytics exports | Scripts | User/audit workflows | Example: vacancy timeseries CSV/QA JSON. Not used for normal startup geometry acquisition. |
| `data/tiles/<z>/<x>/<y>.png` | OSM raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | On-disk basemap PNGs. Disk presence is memoized in memory and can be cleared without deleting PNGs. |
| `data/tiles_topo*/<z>/<x>/<y>.png` | Topographic raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Topographic raster tiles. `data/tiles_topographic` may be used as an alternate/preferred topo source when present. |
| `data/tiles_satellite/<z>/<x>/<y>.png` | Satellite raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Satellite raster tiles, fetched lazily up to the configured native zoom. The dark satellite basemap is a render-time transform over this same cache and does not create a separate tile artifact. |
| `data/tiles_satellite_night/<z>/<x>/<y>.png` | Night satellite raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Night-lights basemap tiles, fetched lazily from the Earth-at-Night source up to its native z7 and displayed only as low/mid-zoom context. High zoom uses dark satellite detail instead of overzooming pixelated night lights. |
| `data/tiles_topo_vector.geojson` | Topographic vector layer | Topo download/build path | Basemap renderer | Optional vector contour/topographic representation. |
| `data/lazy_tile_queue.json` | Persisted lazy tile request queue | Basemap tile queue | Basemap tile queue | Keeps pending visible tile downloads across runs. |
| `data/download_queue.json` | Basemap/data-library download queue | Download queue | Download queue | Persists queued generic downloads. |
| `data/layer_download_queue.json` | Layer download queue | Layer download queue | Layer download queue | Persists queued layer refresh/download work by layer file. |
| `data/layer_ui_state.json` | Layer UI/filter/render state | Layer UI state sync | Startup state load, layer UI | Persists enabled flags, colors, opacity, heatmap settings, filters, and related layer controls. |
| `data/app_settings.json` | App-level settings | Settings UI and shutdown | Startup settings load | Persists Vulkan validation setting, CPU core reservation, basemap toggles/opacities, zoning color mode, and similar app controls. |

Files under `data/cache/` are rebuildable and should be treated as performance artifacts. Files under `data/`, `sources/`, and `data/inbox/` are source or source-adjacent artifacts and should not be cleared as routine cache cleanup unless the intended result is to force reimport or redownload.

Use artifact-health and warm/build commands that operate directly on compiled geometry artifacts by layer file and geometry class. The target system should not expose separate intermediate geometry maintenance stages.

## Startup Layer Scheduling

On startup, the app startup/bootstrap service should enqueue every enabled geometry layer for geometry-artifact acquisition.

That does not mean a text interchange export should ever be reparsed during normal startup. The preferred path is:

```text
render path:
  compiled geometry artifact on disk
    -> runtime maps/loads artifact metadata
    -> runtime uploads artifact-shaped buffers to Vulkan

semantic path:
  data/worldsim.duckdb
    -> query/load required normalized columns
    -> optional DuckDB-derived column sidecar
    -> renderer color/filter/detail buffers
```

The target readiness split should instead be:

```text
compiled geometry artifact present
  -> GPU buffers resident
  -> GPU picking ready
DuckDB present and current
  -> attribute columns ready
  -> filter/choropleth/detail/query workflows ready
```

The frame loop should not compensate for missing compiled geometry by scanning full CPU feature sets. Large geometry layers are either drawn through the retained GPU path or remain non-drawable until their compiled geometry artifact is ready. This includes polygon and polyline outlines: they are retained geometry, not an ImGui fallback workload.

The frame loop should also not compensate for missing DuckDB-derived semantic columns by rebuilding full CPU property bags from canonical binaries. If DuckDB is missing or stale, semantic features should report stale/unavailable status or use an explicitly built derived sidecar with matching signatures. Rebuilding DuckDB or scanning canonical property bags is offline/background work, not startup work.

## Startup Preprocess Contract

Interactive startup has two separate responsibilities:

- report artifact readiness
- ensure required artifacts exist before the main map UI/API starts

The main map UI must not build geometry, DuckDB, or semantic artifacts on the frame-loop path. Startup inspects enabled layers and DuckDB freshness before creating the main UI/API. If required compiled geometry or DuckDB artifacts are missing or stale, startup runs the explicit preprocessing command first, performs a second readiness inspection, and blocks the main UI if any required artifact is still unavailable.

Preprocessing is an explicit CLI operation:

```text
worldsim3 --startup-preprocess [--reserve-cores N]
worldsim3 --build-geometry-duckdb-artifacts [--reserve-cores N]
```

These commands may scan canonical sources and write artifacts. Interactive startup may invoke the same explicit preprocessing command as a blocking pre-main-UI gate when required artifacts are missing or stale. That work is still outside the renderer and outside the frame loop; no degraded main UI should run while required DuckDB semantics or compiled geometry artifacts are missing.

## Compiled Geometry Artifacts

Persistent target path:

```text
data/cache/geometry/<layer-file>.point.bin
data/cache/geometry/<layer-file>.polyline.bin
data/cache/geometry/<layer-file>.polygon.bin
```

The filename must encode the geometry class explicitly. The disk artifact name is part of the contract, not an implementation detail.

Each compiled geometry artifact should contain:

- source signature
- render-shaped vertex and index streams
- feature-reference streams
- per-feature bounds and offset/count records
- per-chunk bounds for culling
- enough metadata for GPU picking and runtime orchestration without CPU geometry reconstruction

Each compiled geometry artifact should not contain:

- full source property bags
- large duplicated text fields
- query-oriented denormalized records better stored in DuckDB

There is no hydration stage in the target design.

There is no triangulation stage in the target design.

Geometry compilation is the only persistent geometry build step:

```text
source layer
  -> geometry compiler
  -> <layer-file>.<geometry-class>.bin
  -> Vulkan upload and GPU picking
```

## Derived Attribute And Column Caches

Derived attribute caches are currently rebuilt in `derived_layer_caches.cpp`. The target contract is that these caches are DuckDB-backed: DuckDB owns the durable columns/tables, and runtime caches are compact session views or optional binary sidecars derived from those DuckDB rows.

The key derived data includes:

- harmonized real-property records
- parcel blocklot by feature arrays
- vacant notice counts by blocklot
- vacant rehab counts by blocklot
- tax lien counts and amounts by blocklot
- tax sale counts and amounts by blocklot
- parcel-level vacancy/tax arrays
- unified parcel records

Invalidation should use propagated geometry-artifact source signatures, not only feature counts. This matters when a source file changes but keeps the same number of rows.

Derived parcel joins should be columnar after semantic load. Runtime code should get compact arrays such as `parcel_blocklot_by_feature`, owner search text, address search text, status, zip, numeric values, and per-feature overlay arrays from DuckDB tables or DuckDB-derived sidecars. Layer enable/disable toggles must not rescan parcel property bags with `getPropertyValue(...)` or `firstDisplayProperty(...)`; toggles should only flip visibility/color state or reuse existing derived arrays.

Current implemented rule for the primary parcel layer:

- runtime parcel semantic arrays and `unified_parcels` now hydrate from DuckDB-backed parcel semantic snapshot queries over `layer_features` and `unified_parcels`
- if DuckDB parcel semantics are unavailable, runtime clears those parcel semantic arrays instead of rebuilding them from canonical parcel property bags as a hidden fallback
- real-property harmonization and some non-parcel semantic families still have follow-on migration work, but primary parcel search/detail/filter counts are no longer supposed to come from startup property-bag scans

Runtime canonical loads must not duplicate property storage. If a layer owns `feature_properties`, property lookup should use the layer-indexed accessors directly; the transient feature-property registry is only for temporary feature vectors that do not have an owning `LayerDef::feature_properties` array. Rebuilding a global pointer registry or copying every property pair during startup is not an acceptable disk-cache strategy because it turns persisted column data back into session-only CPU property bags.

The next target state is to persist the high-traffic derived columns in DuckDB and optionally mirror them into versioned binary sidecars. Startup should prefer opening DuckDB or matching sidecars for fields such as parcel blocklot, owner search text, address search text, normalized status, zip, and numeric value/tax fields. If DuckDB or the sidecar source signatures match, startup must not scan canonical property bags to rebuild those columns.

Required DuckDB-owned startup columns include:

- stable layer id and feature index
- blocklot and other join keys
- owner display/search text
- address display/search text
- status, zip, and normalized categorical fields
- numeric parcel/property values used by filters, choropleths, and detail panels
- parcel-level vacancy/tax/derived counts and amounts
- source signatures/build signatures needed to validate optional sidecars

The unified parcel cache is invalidated when any of these change:

- parcel layer source signature
- parcel feature count
- harmonized real-property size
- parcel vacancy generation
- parcel tax generation

The derived disk artifact currently written by this path is:

```text
data/cache/derived/parcel_vacancy_status.json
```

## DuckDB Analytics Cache

Persistent path:

```text
data/worldsim.duckdb
```

Writer:

```text
duckdb_analytics.cpp -> DuckDbAnalytics::ensureCurrentArtifact()
  -> DuckDbAnalytics::rebuild() only when missing, invalidated, or structurally invalid
```

Reader:

```text
duckdb_analytics.cpp -> DuckDbAnalytics::executeMapQuery()
```

DuckDB is the canonical attribute and query store, not the render cache. The intended end state is direct ingest from persisted canonical inputs and derived artifacts rather than any session-only CPU geometry state.

DuckDB stores extracted, typed feature attributes in `layer_features` and parcel-level property/detail fields in `unified_parcels`. It must not store render geometry, coordinate arrays, WKT/GeoJSON geometry payloads, or full arbitrary property bags by default. The `layer_feature_properties` table is a compatibility placeholder unless a bounded, allowlisted semantic key/value export is explicitly added. DuckDB is the intended home for searchable owner/address/value/detail data, filter inputs, choropleth inputs, and parcel join keys, not the source of startup render geometry.

Normal interactive startup should treat DuckDB as the semantic load source:

```text
data/worldsim.duckdb
  -> layer_features / unified_parcels / derived views
  -> compact runtime semantic arrays
  -> GPU color/filter/selection buffers and detail/query UI
```

Normal interactive startup should not do this:

```text
*.canonical.bin
  -> FeatureRecord vectors
  -> FeatureProperties/property bags
  -> getPropertyValue/firstDisplayProperty scans
  -> runtime semantic arrays
```

That canonical-binary path is allowed for explicit rebuild, validation, and migration tooling. It is not the professional steady-state startup path when `data/worldsim.duckdb` is current.

The database stores `analytics_build_info.source_signature`, which is the combined signature of available source files. `DuckDbAnalytics::needsRebuild()` compares that stored signature to the current source signature instead of relying on database mtime. This avoids false freshness decisions when file timestamps move or a database is copied. The database also stores per-layer analytics signatures in `analytics_layer_state`, so a stale result can be narrowed to the affected layer files instead of forcing a full-table rewrite by default.

DuckDB artifact hydration is isolated in `DuckDbAnalytics::ensureCurrentArtifact()`. That function is the normal provisioning entrypoint: it validates and reuses `data/worldsim.duckdb` when the artifact is current; when the database is structurally valid but stale, it incrementally refreshes the changed `layer_features` rows and then regenerates dependent projections such as `unified_parcels`, `parcel_events`, `geography_feature_collections`, and the analytics metadata tables/views. It calls the unconditional low-level writer `DuckDbAnalytics::rebuild()` only when the output artifact does not exist, the schema is no longer compatible, or structural validation fails.

Runtime layer hydration and DuckDB artifact hydration are separate responsibilities:

- layer/runtime hydration prepares renderable geometry metadata from persisted geometry artifacts
- DuckDB artifact hydration materializes the persisted semantic/query database
- startup and frame-loop code must not invoke DuckDB artifact hydration to satisfy a query or render request
- explicit CLI/provisioning flows may invoke `ensureCurrentArtifact()` after the required source/parcel inputs have been prepared
- explicit destructive/manual maintenance flows may invoke `rebuild()` directly when the operator intentionally wants a full rewrite

DuckDB rebuild is intentionally explicit. The SQL tab exposes a rebuild button, and command-line/offline tools may also rebuild it. The frame loop does not automatically rebuild `data/worldsim.duckdb`, because a full rebuild can write multiple gigabytes and can stall the UI while parcels are still becoming render-ready. When the existing database is structurally valid, stale input should prefer an incremental per-layer refresh over a full drop-and-rebuild of the entire database artifact.

If DuckDB is missing or stale:

- map rendering and GPU upload still proceed from compiled geometry artifacts
- DuckDB-backed search/detail/query/filter/choropleth features report that the cache is unavailable or stale
- the user can rebuild the analytics cache when interactive startup is no longer on the critical path
- runtime must not silently rebuild large property bags from canonical binaries on the startup critical path to hide the stale DuckDB state

Applied rule for parcel history:

- parcel history should be defined by a canonical parcel-event domain pipeline
- a local runtime/derived builder may produce that event stream directly from loaded parcel-related layers
- DuckDB may mirror the same event stream for SQL/query/search convenience
- the UI should not claim that parcel history "requires DuckDB analytics" if the event stream can be assembled from already loaded runtime layers

In other words, DuckDB can be the fast path for parcel history, but not the only legitimate path.

## Clear Cache Behavior

The Performance panel should expose separate clear scopes for:

- compiled geometry disk cache
- derived disk cache
- heatmap aggregate disk cache
- heatmap runtime cache
- tile runtime cache
- tile disk-presence memoization

Clearing compiled geometry data should clear geometry-artifact residency, queued geometry compilation/upload work, runtime provenance signatures, and derived cache state, then re-enqueue enabled layers.

## Important Distinction

The architectural distinction should now be:

- compiled geometry artifacts exist to make rendering and GPU picking cheap
- DuckDB exists to make attributes, joins, detail panels, repeatable filters, choropleths, search, and analytics cheap
- optional derived binary sidecars exist only to make DuckDB-owned hot columns faster to map/load
- runtime memory exists to orchestrate geometry artifacts and DuckDB-backed semantics, not to cache CPU geometry or property bags as a third canonical persistence model

The next implementation step is to remove remaining startup semantic hydration from canonical binaries. Explicit-type compiled geometry artifacts should be the only geometry runtime path for all major geometry families, and DuckDB or DuckDB-derived sidecars should be the only normal startup path for owner/address/blocklot/status/value/filter/choropleth columns.
