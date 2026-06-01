# Maryland Zoning Data Plan

## Objective

Build and maintain a complete, repeatable zoning dataset for every Maryland county-equivalent jurisdiction, with canonical zoning geometries, zone-class metadata, official descriptions, source links, and UI-ready hover/click details.

The program already has working zoning support for several jurisdictions. This plan extends that pattern statewide and standardizes how zoning descriptions are sourced, cached, validated, and displayed.

## Current Coverage

Canonical zoning layers already present in the repository:

- Baltimore City: `zoning.geojson`
- Anne Arundel County: `anne_arundel_county_zoning.geojson`
- Baltimore County: `baltimore_county_zoning.geojson`
- Howard County: `howard_county_zoning.geojson`
- Montgomery County: `montgomery_county_zoning.geojson`
- Prince George's County: `prince_georges_county_zoning.geojson`

Primary metadata file already in use:

- `data/zoning_classes.json`

Relevant UI behavior already expected:

- Zoning layers appear in the layer manifest and left panel under Zoning.
- Click selection opens zone details in the right panel.
- Hover selection shows zone name and brief description.
- Hover/click can fall back to canonical layer features when the GPU pick index is not resident in `LayerDef::features`.

## Target Jurisdictions

Maryland has 24 county-equivalent jurisdictions for this purpose:

- Allegany County
- Anne Arundel County
- Baltimore City
- Baltimore County
- Calvert County
- Caroline County
- Carroll County
- Cecil County
- Charles County
- Dorchester County
- Frederick County
- Garrett County
- Harford County
- Howard County
- Kent County
- Montgomery County
- Prince George's County
- Queen Anne's County
- St. Mary's County
- Somerset County
- Talbot County
- Washington County
- Wicomico County
- Worcester County

Initial gap list, based on current canonical files:

- Allegany County
- Calvert County
- Caroline County
- Carroll County
- Cecil County
- Charles County
- Dorchester County
- Frederick County
- Garrett County
- Harford County
- Kent County
- Queen Anne's County
- St. Mary's County
- Somerset County
- Talbot County
- Washington County
- Wicomico County
- Worcester County

## Source Priority

Use official, canonical sources in this order:

1. County or city ArcGIS REST FeatureServer/MapServer zoning layer.
2. County or city open data portal export for zoning polygons.
3. County or city downloadable GIS package, shapefile, GeoJSON, FileGDB, or zipped geodatabase.
4. County or city official zoning map PDF only when no GIS geometry is available.
5. Maryland state-hosted authoritative mirror only when local government source is unavailable or deprecated.

Do not use scraped third-party map tiles or unofficial GIS mirrors as canonical sources unless they are only used as temporary discovery hints and are replaced before ingestion.

## Geometry Ingestion Plan

For each jurisdiction:

1. Create or update the jurisdiction directory:
   - `data/world/earth/nation_state/us/state_region/md/county_city/<jurisdiction>/layers/`

2. Add a layer entry to the Maryland layer manifest:
   - Name: `<Jurisdiction> Zoning`
   - File: `<jurisdiction>_zoning.geojson`
   - Category: `zoning`
   - Geometry class: polygon
   - Render path: generic polygon GPU
   - Fill enabled by default only when the currently active map context expects it.
   - Hover and inspect enabled.

3. Store source metadata:
   - Official source URL
   - Download URL or ArcGIS REST endpoint
   - Access date
   - Source layer id
   - Source format
   - License or public-use notice if present
   - Source update timestamp when available

4. Normalize geometry:
   - Convert to WGS84 lon/lat.
   - Preserve multipart polygons.
   - Repair invalid rings when safely possible.
   - Drop empty geometries.
   - Preserve source feature id fields.
   - Preserve all zoning code fields.

5. Build canonical binary:
   - `<jurisdiction>_zoning.geojson.canonical.bin`
   - Include feature properties alongside geometry.
   - Ensure GPU polygon artifact generation succeeds.

6. Add regression coverage:
   - Manifest has every jurisdiction.
   - Canonical file exists or source is explicitly marked unavailable.
   - Zoning layers are categorized as Zoning.
   - Zoning layers support hover and click selection.
   - SimCity zoning color assignment receives a non-empty zone key for real features.

## Zone Code Normalization

Each zoning feature must expose a canonical zone key using the existing lookup behavior in `feature_props.cpp`.

Preferred source fields, in order:

- `ZONE`
- `ZONING`
- `ZONED`
- `ZONE_CLASS`
- `ZONE_DIST`
- `DIST_CODE`
- `DISTRICT`
- `CLASS`
- `TYPE`
- jurisdiction-specific equivalent fields

For every new layer:

1. Inspect source attributes.
2. Add jurisdiction-specific mapping only if generic field lookup is insufficient.
3. Preserve raw source fields.
4. Store normalized code in metadata if needed.
5. Record aliases in `data/zoning_classes.json` so the same district can be resolved consistently.

## Zone Description Subplan

Goal: every zone shown on hover or click should have:

- Zone code
- Human-readable label
- Brief description
- Source name
- Source URL
- Optional PDF URL
- Optional extracted official text
- Optional source page or section citation

### Description Source Priority

Use canonical official descriptions in this order:

1. County zoning code webpage with district text.
2. County zoning code PDF or official ordinance PDF.
3. County zoning report PDF per district.
4. Official GIS layer attributes if they include district descriptions.
5. Official planning department zoning guide.

Avoid generated summaries as the only record. Generated summaries may be stored only as a convenience field when the official text or official URL is also retained.

### Metadata Schema

Extend `data/zoning_classes.json` toward a jurisdiction-aware structure. Existing flat keys may remain for backward compatibility, but new data should support:

```json
{
  "jurisdictions": {
    "baltimore_county": {
      "source_name": "Baltimore County Zoning Reports",
      "source_url": "https://...",
      "zones": {
        "DR 3.5": {
          "label": "DR 3.5 - Density Residential, 3.5 Units/acre",
          "brief_description": "Officially summarized district purpose and principal uses.",
          "official_text": "Short extracted excerpt from the official source.",
          "source_url": "https://...",
          "source_document": "DR 3.5.pdf",
          "source_section": "District report heading or code section",
          "aliases": ["DR3.5", "DR-3.5"],
          "color_hex": "#..."
        }
      }
    }
  }
}
```

If retaining the current flat `data/zoning_classes.json` during transition, use namespaced keys where conflicts are possible:

- `baltimore_county::DR 3.5`
- `baltimore_city::R-8`
- fallback unnamespaced key only when there is no ambiguity.

### Description Extraction Workflow

For each jurisdiction:

1. Enumerate unique zoning codes from the canonical layer.
2. Locate the official zoning ordinance, zoning district table, or zoning report source.
3. Download official documents into a cache path:
   - `data/cache/zoning_reports/<jurisdiction>/`
4. Extract text using a deterministic toolchain:
   - HTML parser for web code pages.
   - PDF text extractor for PDFs.
   - OCR only when the official PDF has no embedded text.
5. Match extracted sections to zone codes:
   - Exact code match.
   - Alias match.
   - Heading/table-row match.
   - Manual review list for unmatched codes.
6. Produce a concise `brief_description` from official text:
   - Prefer official purpose statement.
   - Otherwise use permitted-use summary.
   - Keep it short enough for hover.
7. Store source URL and source section with each description.
8. Validate every zone code in the layer has metadata or is explicitly marked unresolved.

### UI Availability Requirements

Hover:

- Always show label/code and brief description if metadata exists.
- Keep hover tooltip inside visible map bounds.
- Fall back to canonical feature properties when the picked feature is not resident in memory.
- If description is missing, show the code and a short "No official description loaded" message only if useful; do not pretend there is a description.

Click:

- Open the right-panel zone inspector.
- Show label/code, brief description, source fields, source URL, and official document link.
- Preserve raw source attributes for auditability.
- If the description comes from a PDF, expose the PDF link and source section if known.

Regression tests:

- Hover and click lookup use the same zone-code normalization.
- Canonical fallback works for zoning feature indices outside in-memory `LayerDef::features`.
- Zone metadata lookup handles jurisdiction-specific codes.
- Ambiguous zone codes do not incorrectly reuse another jurisdiction's description.

## Data Quality Gates

Every zoning layer must pass:

- Source URL is official and reachable at ingestion time.
- Canonical layer has at least one polygon feature.
- At least 95% of features have a non-empty normalized zoning code.
- 100% of unique normalized codes are either described or listed in an unresolved report.
- Geometry bounds intersect the expected jurisdiction.
- Layer category is Zoning.
- Layer appears under Zoning in the left panel manifest.
- Hover selection and click selection both resolve zone details.
- GPU picking can return a feature index for representative visible polygons.

## Deliverables

Per jurisdiction:

- Canonical zoning geometry file.
- Source metadata JSON.
- Unique zone-code inventory.
- Zone description entries.
- Unresolved code report, if any.
- Regression test fixture or manifest assertion.

Statewide:

- Updated Maryland layer manifest.
- Updated `data/zoning_classes.json` or replacement jurisdiction-aware schema.
- Zoning source audit report.
- UI smoke test for hover and click details.

## Implementation Phases

### Phase 1: Inventory

- Generate current source inventory for all 24 jurisdictions.
- Confirm existing six layers still point to official sources.
- Produce a missing-source table for the 18 uncovered jurisdictions.

### Phase 2: Geometry Acquisition

- Add remaining county zoning sources in batches of 3 to 5 jurisdictions.
- Build canonical binaries and GPU polygon artifacts.
- Keep each batch independently testable.

### Phase 3: Description Acquisition

- Extract zone descriptions for already-loaded jurisdictions first.
- Then process new counties as geometry lands.
- Emit unresolved-code reports after every run.

### Phase 4: Schema Upgrade

- Add jurisdiction-aware metadata lookup while retaining backward compatibility.
- Update hover/click detail code to prefer jurisdiction-specific metadata.
- Add tests for conflicting codes across jurisdictions.

### Phase 5: Validation and Maintenance

- Add a command-line audit task:
  - list zoning coverage by jurisdiction
  - list source freshness
  - list missing descriptions
  - list unresolved aliases
- Add scheduled or manual refresh instructions.
- Preserve old source snapshots so zoning changes can be diffed.

## Open Engineering Questions

- Whether `data/zoning_classes.json` should be migrated in place or replaced by `data/zoning_metadata.json` with a compatibility loader.
- Whether descriptions should store full official text or only a short extracted excerpt plus source link.
- Whether source freshness should be checked during startup, a CLI audit, or a separate data maintenance command.
- Whether all zoning layers should be enabled by default only within the selected county context, to avoid visual clutter statewide.

## Acceptance Criteria

The work is complete when:

- All 24 Maryland county-equivalent jurisdictions have zoning geometry or an explicit official-source-unavailable record.
- Every zoning layer appears under Zoning in the left panel.
- Every zoning layer supports GPU hover/click selection.
- Hover shows zone name/code and brief description when available.
- Click opens a right-panel zone inspector with official source details.
- All unique zone codes have metadata or appear in an unresolved report.
- Regression tests prevent future county zoning layers from becoming invisible, uncategorized, unselectable, or undescribed.
