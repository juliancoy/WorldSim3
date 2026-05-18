# Vulkan Aggregate Pipeline Plan

## Goal
Move expensive aggregate generation, especially parcel/value choropleth aggregation, into a dedicated Vulkan pipeline while preserving the current CPU path as the correctness fallback. Aggregates should be generated only when their data/settings/filter key changes, then reused as a texture during pan and normal rendering.

## Current State
- Aggregate caching is keyed by layer/filter/settings and no longer by pan/viewport.
- Vector aggregate cells are stored in world coordinates and projected at draw time.
- `Median Choropleth` is area-aware on CPU: polygon cells are sampled by cell center with hole handling.
- Smooth methods can already produce a raster/texture handoff through `HeatmapRenderData::raster` and `uploadRgbaTexture`.
- Existing Vulkan helpers are focused on texture upload and ImGui presentation, not compute resource management.

## Howard County Splat Benchmark
Use the GPU aggregate harness to profile the full Howard County parcel extent without the UI/render loop:

```bash
./build/worldsim3_gpu_aggregate_harness \
  --howard \
  --input data/world/earth/nation_state/us/state_region/md/county_city/howard_county/layers/howard_county_parcels.geojson \
  --jurisdiction "Howard County" \
  --raster 512 \
  --repeats 3
```

The harness loads all matching features, computes the full input extent, emits one heat sample per parcel centroid, runs the real `buildGpuSplatAggregate` compute path, and separately times the CPU separable blur pass currently used after GPU binning. Useful switches:

```bash
--raster 1024       # square raster dimensions
--repeats 10        # repeat aggregate timing
--sigma 6           # blur sigma in raster pixels
--no-cpu-blur       # isolate GPU binning/readback cost
--input PATH        # use regional_parcels.geojson or a staging layer
--jurisdiction NAME # e.g. HowardCounty for regional_parcels.geojson
```

Baseline on the current machine with `data/world/earth/nation_state/us/state_region/md/county_city/howard_county/layers/howard_county_parcels.geojson` after persistent GPU buffers/descriptor/command-buffer reuse:

| Raster | Samples | GPU bin/readback avg | CPU blur avg | Total avg |
| --- | ---: | ---: | ---: | ---: |
| 512x512 | 110,598 | ~205 ms | ~132 ms | ~337 ms |
| 512x512 no blur | 110,598 | ~202 ms | 0 ms | ~202 ms |
| 1024x1024 | 110,598 | ~749 ms | ~649 ms | ~1398 ms |
| 1024x1024 no blur | 110,598 | ~766 ms | 0 ms | ~766 ms |

Persistent buffers removed the per-call allocation churn but only modestly improved timings. The current path is now dominated by queue wait/readback and CPU blur. Improving the splat algorithm should focus on GPU-side blur/output image generation, avoiding full readback when the UI only needs a texture, and keeping the aggregate result resident across frames.

## Proposed Files
- `aggregate_vulkan.h`
- `aggregate_vulkan.cpp`
- `aggregate_vulkan_shaders.h` or `shaders/aggregate_*.comp`

Prefer source GLSL files plus CMake-time `glslangValidator` compilation when available. Keep a fallback embedded SPIR-V/header path only if packaging requires it.

## Public API Sketch
```cpp
struct AggregateGpuFeature {
    float min_x, min_y, max_x, max_y;
    float value;
    uint32_t first_vertex;
    uint32_t vertex_count;
    uint32_t first_ring;
    uint32_t ring_count;
};

struct AggregateGpuRing {
    uint32_t first_vertex;
    uint32_t vertex_count;
};

struct AggregateGpuRequest {
    uint64_t key;
    int algo;
    float cell_px;
    float percentile_clip;
    int width;
    int height;
    float world_min_x;
    float world_min_y;
    float world_max_x;
    float world_max_y;
    std::span<const AggregateGpuFeature> features;
    std::span<const AggregateGpuRing> rings;
    std::span<const ImVec2> vertices;
};

struct AggregateGpuResult {
    uint64_t key;
    HeatmapRaster raster;
    TileTexture texture;
    bool texture_valid = false;
};

class AggregateVulkanRenderer {
public:
    bool initialize();
    void shutdown();
    bool isAvailable() const;
    std::optional<AggregateGpuResult> render(const AggregateGpuRequest& request);
};
```

## Pipeline Architecture
1. **Input Build On CPU**
   - Convert loaded feature rings to world-space vertices once per aggregate key.
   - Build feature metadata buffer with extents, normalized value, and ring ranges.
   - Build ring metadata buffer for polygon holes and multipolygon rings.
   - Build output image dimensions from requested world extent and cell size.

2. **GPU Storage Buffers**
   - `FeatureBuffer`: one record per parcel/feature.
   - `RingBuffer`: one record per polygon ring.
   - `VertexBuffer`: world-space ring vertices.
   - `CellAccumBuffer`: per-cell accumulation storage.
   - `CellCountBuffer` or compact per-cell value list/indexing if median is exact.

3. **Compute Passes**
   - Pass A: clear output accumulation buffers.
   - Pass B: polygon coverage accumulation.
   - Pass C: normalize/ramp to RGBA image.
   - Optional Pass D: generate mip levels or smooth post-process for non-choropleth methods.

4. **Texture Handoff**
   - Output should be a `VK_FORMAT_R8G8B8A8_UNORM` storage image.
   - Transition storage image to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
   - Register image view with `ImGui_ImplVulkan_AddTexture` using existing sampler.
   - Store in aggregate cache as `TileTexture` equivalent or a dedicated texture wrapper.

## Choropleth Algorithm
### Initial GPU Version
Use cell-center-in-polygon coverage, matching current CPU semantics.

For each feature:
- Compute covered cell bounds from feature world extent.
- For each candidate cell center, test point-in-polygon.
- If inside outer ring and outside holes, accumulate the feature value into the cell.

### Median Handling
Exact median on GPU is non-trivial because each cell may receive a variable number of values.

Preferred staged approach:
1. **Phase 1:** use mean or clipped mean on GPU for fast preview, CPU exact median fallback for final correctness.
2. **Phase 2:** implement approximate median using fixed-bin histograms per cell.
3. **Phase 3:** optional exact median with prefix-summed per-cell lists if accuracy requirements justify complexity.

For property values, fixed-bin histogram median is usually acceptable if bins are based on layer percentile-clipped value domain.

## Shader Design
### `aggregate_clear.comp`
- One invocation per output cell.
- Clears count, sum, histogram bins, and RGBA output.

### `aggregate_polygon.comp`
- Workgroups process features or feature/cell tiles.
- Reads feature extents, rings, vertices.
- Tests candidate cell centers against polygon rings.
- Writes accumulation using atomics.

### `aggregate_resolve.comp`
- One invocation per output cell.
- Computes statistic: count, mean, histogram median, or density.
- Applies blue-yellow-red ramp.
- Writes final RGBA storage image.

## Resource Ownership
Create an `AggregateVulkanRenderer` owning:
- Descriptor set layout
- Descriptor pool or descriptor allocator
- Pipeline layouts
- Compute pipelines
- Command pool and command buffer
- Reusable buffers sized to peak request
- Output image/image view/memory
- Optional staging/readback buffers for debug

Do not reuse tile upload command buffers directly. The aggregate module should own its command pool to avoid coupling with texture upload and frame rendering.

## Synchronization
- Submit compute work on `g_Queue` initially, since the app currently uses one queue family.
- Use image and buffer barriers between clear, accumulate, resolve, and shader-read transitions.
- Use a fence for completion before handing the texture to ImGui.
- Later optimization: async compute queue if available.

## Cache Integration
Current aggregate cache should store GPU-produced textures by the same aggregate key.

Rules:
- If key exists, draw cached texture/cells without recompute.
- If key missing and GPU renderer available, enqueue/render GPU aggregate.
- If GPU fails, fall back to CPU aggregate generation.
- Changing filters, layer enable state, normalization, method, zoom, or cell size invalidates key.
- Panning must not invalidate key.

## UI Integration
Add optional diagnostics:
- GPU aggregate enabled/disabled
- Last aggregate path: CPU / GPU
- GPU aggregate build time
- Output texture size
- Number of features/rings/vertices submitted
- Fallback reason, if any

## Implementation Order
1. Add `aggregate_vulkan.h/.cpp` with lifecycle stubs and CPU fallback status.
2. Add CMake shader compilation support using `glslangValidator`.
3. Implement Vulkan buffer/image helpers local to `aggregate_vulkan.cpp`.
4. Implement clear and resolve shaders with synthetic data tests.
5. Implement polygon coverage shader for center-in-polygon sampling.
6. Wire `Median Choropleth` requests to GPU path behind a feature flag.
7. Preserve CPU exact median fallback.
8. Add profiling counters and `/status` exposure.
9. Stress test with property-value parcels at `cell_px=2`.

## Risks
- Exact median is expensive on GPU without histogram approximation or variable-length per-cell lists.
- Cell size `2 px` can create many output cells and high polygon coverage cost.
- Very large polygons can dominate compute unless feature/cell tiling is carefully bounded.
- Descriptor lifetime must not conflict with ImGui texture removal.
- Shader compilation availability differs by deployment environment.

## Non-Goals For First Patch
- Full GPU replacement for all aggregate algorithms.
- Async multi-queue scheduling.
- Exact GPU median for arbitrary value distributions.
- Removing the CPU aggregate path.
