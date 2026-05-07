# Data Source Organization

## Tiers

`official-structured`
: Direct official GeoJSON, FeatureServer, or CSV sources with stable fields and machine-readable geometry or parcel join keys. These are suitable for automated ingestion.

`official-api`
: Official API/catalog sources that require query logic, pagination, year selection, or post-processing before they should become app layers.

`official-document`
: Official PDFs, budget books, archive pages, and scanned records. These can extend history further back, but they require OCR/table extraction, address matching, and confidence scoring.

## Current Organized Phases

`must-have`
: Compact operational baseline.

`nice-to-have`
: Medium-size contextual layers.

`heavy-data`
: Large geospatial layers that should stay out of default checkout and Git history.

`extended-events`
: Parcel event sources such as foreclosure, receivership, work orders, impact investment areas, and CIP point allocations.

`historical-high-quality`
: Older official structured records and additional capital-project layers. This currently includes 2015-2018 permits and DPW/CIP project layers.

`capital-flows`
: Nonprofit, lending, federal award, LIHTC, CRA, and related capital flow sources. Some entries are intentionally metadata/API placeholders until a dedicated fetcher exists.

## Historical Ingestion Rule

Historical records should be normalized into parcel-event style fields before analytics:

- `BLOCKLOT` or spatial/address join
- `event_type`
- `event_date`
- `amount_usd`
- `source_name`
- `source_year`
- `confidence`

Records without a direct parcel key should be joined spatially or geocoded and marked with a confidence score instead of being treated as exact.
