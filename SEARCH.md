# Search And Filtering Plan

## Current State

- Address search has a prominent `Address Search` field in the `Record Filters` panel.
- Pressing Enter or clicking `Locate` searches parcel/property addresses, recenters the map, zooms in, selects the matching parcel, and opens parcel details.
- Address matching now normalizes case, punctuation, ordinal suffixes, directional words, and common street suffixes.
- Address filtering uses the same improved matching path and searches both parcel fields and joined Real Property Information fields.
- Existing filters include record year, block/lot, status, owner, ZIP, selected owners, and crime type/year filters.

## Goal

Make search behave like a property-finding workflow, not just a layer filter:

- Find a property by address or partial address.
- Jump to and select the best match.
- Show alternate matches when the query is ambiguous.
- Combine text filters with a map-drawn polygon filter.
- Keep rendering, heatmaps, overlays, and aggregate caches consistent with filter changes.

## Improved Search

### Short Term

- Add a small result list below `Address Search` after locate/search:
  - Best 5-10 matching parcels.
  - Display address, block/lot, owner, ZIP, and score/match reason.
  - Selecting a result recenters and opens parcel details.
- Add exact/near match feedback:
  - `Exact address match`
  - `Normalized address match`
  - `All query terms matched`
  - `No property address found`
- Include owner and block/lot fallback in the same search box only when address search fails.
- Cache normalized address strings per parcel so repeated searches do not rescan and normalize every frame.

### Medium Term

- Build a `PropertySearchIndex` at hydration time or after parcel/real-property join readiness:
  - `parcel_feature_idx`
  - normalized property address
  - raw display address
  - normalized owner
  - normalized block/lot
  - ZIP
  - centroid lon/lat
- Rebuild the index when parcel or real-property layer feature counts/signatures change.
- Move search scoring out of the run-loop include and into a small module, for example:
  - `property_search.h`
  - `property_search.cpp`
- Add prefix/token scoring so `123 char` ranks above unrelated token-only matches.
- Add optional fuzzy tolerance for small typos after exact normalized matching is attempted.

## Polygon Filter

### User Experience

- Add a `Spatial Filter` section in `Record Filters`.
- Modes:
  - `Off`
  - `Draw Polygon`
  - `Use Selected Polygon Layer Feature`
  - `Clear`
- In `Draw Polygon` mode:
  - Click map to add vertices.
  - Double-click or press Enter to close polygon.
  - Escape cancels.
  - Backspace removes last vertex.
  - Show the in-progress polygon with translucent fill and visible vertex handles.
- Once active:
  - Show area/vertex count.
  - Show count of matching parcels/features.
  - Offer `Zoom To Polygon` and `Clear Polygon`.

### Filtering Semantics

- For polygon/parcel features:
  - Include if feature centroid is inside polygon by default.
  - Add an option for `intersects polygon` for parcel polygons where precision matters.
- For point features:
  - Include if point is inside polygon.
- For line features:
  - Initially use extent intersection plus sampled vertices.
  - Later add segment intersection if needed.
- Polygon filter should combine with existing filters using AND semantics:
  - `passes_text_filters && passes_owner_filter && passes_polygon_filter`
- Polygon filter should apply to:
  - regular layer drawing
  - vacancy/tax overlays
  - heatmap sample collection
  - aggregate cache keys
  - filtered summary counts

### Implementation Steps

1. Add runtime state in `app_main_loop.cpp`:
   - `bool polygon_filter_enabled`
   - `bool polygon_filter_drawing`
   - `std::vector<ImVec2> polygon_filter_vertices_lonlat`
   - `bool polygon_filter_intersects_mode`
   - `std::string polygon_filter_status`

2. Add geometry helpers:
   - `pointInPolygonFilter(lon, lat, vertices)`
   - `featureCentroid(fg)`
   - `featureIntersectsPolygonFilter(fg, vertices)`
   - Reuse existing `pointInRing`/`pointInFeature` where appropriate.

3. Add drawing interaction in the map panel:
   - Convert mouse clicks from screen to lon/lat using existing map projection helpers.
   - Avoid adding vertices when ImGui wants mouse capture from another control.
   - Draw polygon overlay after basemap and before hover tooltip.

4. Integrate with `feature_passes_filters`:
   - Early return true when polygon filter is disabled or has fewer than 3 vertices.
   - Otherwise test each feature against the active polygon.

5. Add polygon state to filter/cache keys:
   - Hash enabled flag.
   - Hash vertices rounded to stable precision.
   - Hash centroid/intersection mode.
   - This prevents stale heatmap/aggregate results when the polygon changes.

6. Add UI controls:
   - `Start Drawing`
   - `Finish`
   - `Undo Point`
   - `Clear`
   - `Centroid` / `Intersects` toggle.

7. Add verification:
   - Build app.
   - Draw a polygon around a known block and confirm parcel count drops.
   - Confirm address locate still works with polygon filter off.
   - Confirm address filter + polygon filter combine correctly.
   - Confirm heatmaps/overlays change when polygon filter changes.

## Data And Performance Notes

- Use spatial indexes before expensive polygon tests:
  - Query features whose extents overlap the polygon bounding box.
  - Then run point-in-polygon or intersection checks.
- Keep the first implementation centroid-based for speed and predictability.
- Add true polygon intersection only after UX and cache invalidation are stable.
- Avoid recomputing normalized addresses or polygon bounds inside tight draw loops.

## Suggested Order

1. Extract address search/index code into a small module.
2. Add search result list and cached search index.
3. Add polygon filter runtime state and UI.
4. Add centroid-based polygon filtering to `feature_passes_filters`.
5. Add polygon drawing overlay and map interactions.
6. Add polygon state to aggregate/filter cache keys.
7. Add optional intersects mode.
