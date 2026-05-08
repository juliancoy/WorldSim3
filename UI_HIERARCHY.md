# UI Hierarchy

## Top-Level Windows
- Layers and Controls
- Map
- Record Filters
- Download Queue (conditional)
- Gear Panel (conditional)

## Layers and Controls
- Header actions: Gear, Library
- Basemap controls
  - Source selector
  - OSM / Topographic entries
  - Download (`D`) actions
  - Layer display settings (`?`)
- Basemap Layers
  - OpenStreetMap
  - Topographic
- Category sections
  - Housing
  - Public Health
  - Safety
  - Infrastructure
  - Zoning
- Zoning Filters
  - Group toggles
  - Per-zone toggles
- Performance and Stats

## Category Row Controls (per layer)
- Download (`D`) when dataset missing
- Visibility (`V`)
- Layer settings (`⚙`)
- Label / status / feature count

## Record Filters
- Filters tab
- Vacancy-Parcel tab
- Gradient tab
- Owners tab

## REST Access
- `GET /ui` returns live layer state and hierarchy tree.
- `GET /ui?include_hierarchy=1` forces inclusion of `ui_hierarchy` and `ui_hierarchy_markdown`.
- `GET /ui?action=click|move|scroll` injects pointer actions.
- `GET /ui?action=download_layer&file=<layer_file>` queues a direct dataset download by layer file.
