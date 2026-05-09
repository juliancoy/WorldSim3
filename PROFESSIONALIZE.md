# PROFESSIONALIZE

Plan for raising `worldsim3` from an experimental parcel/GIS viewer into a defensible local-first parcel intelligence and urban simulation tool.

## 1) Product Positioning

The project is closest to a local-first hybrid of:

- Urban analytics GIS platforms
- Parcel/property intelligence systems
- Civic data observatories
- DuckDB/PostGIS-style geospatial analytics pipelines
- Lightweight urban simulation/scenario tools

The professional target is:

**A local-first parcel intelligence and urban simulation GIS.**

## 2) Highest-Impact Work

### 2.1 Data Provenance

Every important derived field should carry:

- Source dataset
- Source URL
- Source vintage / update timestamp
- Download timestamp
- Transform name/version
- Join method
- Join confidence

Implementation targets:

- `UnifiedParcelRecord`
- `unified_parcels` DuckDB table
- future `parcel_events`
- future `parcel_value_events`

### 2.2 Canonical Data Model

Keep moving from ad hoc layer logic to stable tables/records:

- `unified_parcels`
- `parcel_events`
- `parcel_value_events`
- `owners`
- `owner_classes`
- `query_layers`
- `scenarios`

Rule: UI and SQL should consume canonical records first, raw layers only when inspecting source details.

### 2.3 Quality And Confidence

Expose uncertainty instead of hiding it.

Add fields for:

- Exact blocklot join
- Address fuzzy join
- Spatial join
- Missing source fields
- Stale source warning
- Conflicting owner/value fields
- Value calculation method

Example:

```cpp
enum class ParcelJoinMethod {
    None,
    ExactBlocklot,
    AddressFuzzy,
    SpatialCentroid
};
```

### 2.4 Reproducible Analytics

Every derived metric should be reproducible from recorded inputs:

- Formula/version
- Parameter set
- Input table versions
- Run timestamp
- Transform name

Example current value model:

1. Prefer `TAXBASE` / `ARTAXBAS`
2. Else `CURRLAND + CURRIMPR`
3. Else `SALEPRIC`

Store this as `value_method`, not just `current_value`.

### 2.5 Reportable Outputs

Professional users need outputs beyond the map:

- CSV export
- GeoJSON export
- DuckDB/Parquet export
- Parcel report
- Owner portfolio report
- Query result export
- Scenario report

## 3) User-Facing Professionalism

### 3.1 Query Layers

Make SQL query layers first-class:

- Saved queries
- Named colors
- Result counts
- Legend entries
- Query history
- Export query results
- Hover explanation: matched by query/layer/blocklot/owner

Current foundation:

- `QueryMapLayer`
- `FilterResultSet`
- SQL tab query entrypoint

### 3.2 Legend And Symbology

Add a proper map legend:

- Active layers
- Zoning colors
- Query colors
- Value gradient scales
- Opacity controls
- Selected parcel outline
- Disabled/stale source indicators

This is one of the fastest visible improvements.

### 3.3 Workflow Design

Move from “tabs of controls” toward task workflows:

- Find parcel
- Inspect parcel
- Inspect owner
- Run query
- Compare scenario
- Export report

Capabilities already exist; the workflow needs to make them obvious.

## 4) Testing And Validation

Add tests for:

- Blocklot normalization
- Owner normalization
- Unified parcel construction
- Current value calculation
- Query result matching
- Filter behavior
- SQL-to-map coloring

Professional credibility depends on stable calculations.

## 5) Documentation Set

Create focused docs:

- `DATA_MODEL.md`
- `PROVENANCE.md`
- `QUERY_LAYERS.md`
- `UNIFIED_PARCELS.md`
- `VALUE_MODEL.md`
- `SCENARIOS.md`

Each doc should describe source fields, transformation logic, and known limitations.

## 6) Implementation Roadmap

### Phase 1: Trust Foundation

- Add `value_method` to `UnifiedParcelRecord`.
- Add `join_method` and `join_confidence` to `UnifiedParcelRecord`.
- Add source/provenance fields to `unified_parcels`.
- Document `VALUE_MODEL.md`.
- Add unit tests for parcel value calculation.

### Phase 2: Query Professionalization

- Add query-layer legend.
- Add saved query definitions.
- Add result-count display by query layer.
- Add export for query result rows.
- Add hover explanation for query matches.

### Phase 3: Data Model Expansion

- Add `parcel_events`.
- Add `parcel_value_events`.
- Normalize sale, assessment, vacancy, permit, lien, and tax events into event rows.
- Use `parcel_value_events` for historical value charts.

### Phase 4: Reporting

- Add parcel report export.
- Add owner portfolio report export.
- Add scenario report export.
- Add CSV/GeoJSON/Parquet export paths.

### Phase 5: Simulation

- Add Monte Carlo scenario definitions.
- Run simulations from `unified_parcels` and `parcel_value_events`.
- Store scenario outputs in DuckDB.
- Render scenario results through query layers.

## 7) Immediate Next Best Step

Implement provenance and value-method fields in the unified parcel model:

- `value_method`
- `join_method`
- `join_confidence`
- `source_dataset`
- `source_updated_at`

Then expose those fields in:

- Parcel Info
- `unified_parcels` DuckDB table
- SQL tab examples
