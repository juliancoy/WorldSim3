# New Visual Models Implementation Plan

## 1. Add a heatmap config model
- Create a `HeatmapSettings` struct (algorithm, bandwidth, sigma, clip percentile, adaptive bandwidth, multires settings, cell size).
- Add per-layer overrides (enable, max zoom, gradient mode, optional algorithm override).

## 2. Persist settings
- Extend app settings/layer UI state I/O to save/load global and per-layer heatmap settings.
- Add schema versioning + backward-compatible defaults.

## 3. Refactor heatmap pipeline interface
- Introduce a common `IHeatmapRenderer`-style abstraction (or switch-based module boundary) with uniform inputs/outputs.
- Keep current grid method as baseline implementation.

## 4. Implement algorithm backends (CPU first)
- `Grid`: existing path cleanup + normalization improvements.
- `KDE`: Gaussian kernel accumulation with zoom-adaptive bandwidth.
- `Hex`: axial hex binning + normalization.
- `Multi-res`: pyramid levels + blended sampling across zoom.

## 5. Implement GPU splat + blur backend
- Add offscreen density targets.
- Point splat pass (additive accumulation).
- Separable blur pass (horizontal/vertical sigma).
- Normalize + color-map compose pass.
- Add capability fallback to CPU path.

## 6. Add normalization and color-mapping stage
- Centralize normalization modes (max, percentile clip).
- Respect per-layer “apply gradient colors” and single-hue intensity continuum behavior.
- Ensure consistent legend scale across algorithms.

## 7. Wire settings to runtime selection
- Use global algorithm selector and per-layer heatmap toggles/max zoom in render dispatch.
- Support mixed-mode rendering (some layers heatmap, some vector) in same frame.

## 8. Performance and cache strategy
- Add tile/viewport-aware caching for bins/density textures.
- Recompute only when camera/layer/filter/settings materially change.
- Add quality presets (fast/balanced/high).

## 9. UX polish
- Show algorithm-specific controls conditionally.
- Add inline tooltips for each option.
- Add “reset heatmap defaults” and “copy settings to all layers”.

## 10. Validation and QA
- Visual regression snapshots across zoom levels.
- Stress tests on large datasets.
- Verify parity for current workflows (vacants/crime/parcel overlays).
- Add telemetry counters (frame time, heatmap build time, cache hit rate).

## 11. Rollout order
- Phase 1: persistence + pipeline abstraction + cleaned grid.
- Phase 2: KDE + hex + multires (CPU).
- Phase 3: GPU splat + blur + performance tuning.
- Phase 4: UX polish + final QA.
