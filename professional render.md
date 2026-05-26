# Professional Render Strategy

## Question

Should WorldSim pre-render parcel and overlay geometry into images, especially as the number of parcel files grows?

## Short Answer

Yes, but only as a cache layer for specific zoom/viewport products. It should not replace the canonical geometry pipeline or the GPU vector renderer.

For many millions of parcels, a professional renderer normally uses a hybrid approach:

- Canonical vector data remains the source of truth.
- Compiled geometry artifacts remain the source of truth for interactive rendering, picking, filtering, and inspection.
- Raster or image tiles are generated as derived cache artifacts for fast visual display at stable zoom levels.
- Vector rendering remains available for high zoom, selection, hover, exact inspection, styling changes, and debug.

Pre-rendering everything into one large image does not make sense. Pre-rendering into route-aware, zoom-aware tiles can make sense.

## Why A Single Pre-Rendered Image Is Not Enough

A single image is fast to draw, but it loses the properties that make parcels useful:

- It cannot support exact parcel picking without a separate pick index.
- It cannot respond cleanly to filters, choropleths, owner queries, or selected jurisdictions.
- It becomes stale whenever source data, styling, filters, or route logic changes.
- It has poor memory behavior if the covered area and zoom level are large.
- It hides geometry corruption instead of exposing it.

For parcel workflows, an image-only path is a display optimization, not a data model.

## Recommended Architecture

Use three explicit render products.

1. Canonical Source

This is the live data contract:

- Source GeoJSON or canonical binary.
- Stable feature identity.
- Source signature.
- County provenance.
- Layer manifest metadata.

This is the only place data correctness should be decided.

2. Compiled Geometry Artifact

This is the interactive render contract:

- Route-aware filename, for example `baltimore_county_parcels.geojson.parcel_gpu.polygon.bin`.
- Vertices, fill indices, line indices, chunks, feature refs, extents.
- Validated against the source signature.
- Used for GPU upload, picking, debug, and high-zoom parcel inspection.

This must stay vector-based.

3. Raster Tile Cache

This is the fast visual cache:

- Generated from the same compiled geometry artifact.
- Named by source signature, render route, style key, zoom, tile x/y.
- Safe to delete and rebuild.
- Used only when the view/style/filter state exactly matches the tile key.

Example cache shape:

```text
data/cache/render_tiles/
  baltimore_county_parcels.geojson/
    parcel_gpu/
      source_<signature>/
        style_<style_key>/
          z14/
            4821_6140.png
```

The current KISS implementation writes an offline PPM tile first:

```bash
worldsim3 --render-polygon-tile LAYER_FILE Z X Y
```

That command reads the route-aware compiled polygon artifact, projects triangles into a 256x256 slippy-map tile, and writes the result under `data/cache/render_tiles/...`. This is intentionally a derived cache product; runtime UI consumption can be added after the cache key and artifact provenance are stable.

## When Raster Tiles Make Sense

Raster tiles are useful when:

- The map is zoomed out and millions of polygons are smaller than a few pixels.
- The style is stable, such as static parcel fill/outline.
- The user is panning over a large area.
- The same county/layer is repeatedly viewed.
- The render output is purely visual.

Raster tiles are not enough when:

- The user is selecting parcels.
- The user is hovering or inspecting exact features.
- The layer is filtered by owner, value, status, jurisdiction, or query result.
- The style changes frequently.
- The system is validating geometry correctness.

## Professional Rule

Never let raster tiles become the source of truth.

Raster tiles should be treated like GPU buffers: disposable derived artifacts. If deleting the tile cache changes application correctness, the architecture is wrong.

## Practical Rendering Policy

Use this policy:

- Zoomed out: draw raster tiles when available, fall back to chunked GPU vector draw.
- Mid zoom: draw raster base plus vector outlines or selected features.
- High zoom: draw vector parcels directly from compiled geometry artifacts.
- Interaction pass: always use vector/pick artifacts, not color pixels.
- Debug mode: expose exact source path, geometry artifact path, render route, tile path, source signature, and style key.

## Why This Fits Many County Parcel Files

Many county files do not require many special renderers. They require one routing system and multiple cache products.

The current direction should be:

- One source of truth for render route classification.
- Route-aware artifact names.
- County-independent geometry compilation.
- Optional raster tile generation from those artifacts.
- Validation that raster and vector paths are derived from the same source signature.

That allows Baltimore City, Baltimore County, and every other county to be treated the same way while still allowing optimized display.

## Next Step

Build the raster path only after the vector route is correct.

The immediate professional sequence is:

1. Finish enforcing a single render-route source of truth.
2. Ensure all parcel counties use route-aware geometry artifacts.
3. Add screenshot/headless tests for representative counties.
4. Add runtime use of raster tiles as an optimization layer.
5. Keep invalidation keyed by source signature, render route, style key, zoom, and tile coordinate.

Pre-rendering is a good optimization. It is not a replacement for fixing the parcel render pipeline.
