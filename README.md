# Baltimore Vulkan Economic Map

Pure C++/Vulkan app (no Qt) with Vulkan-rendered UI and local Baltimore economic/public-data layers.

## Stack

- Vulkan renderer
- GLFW windowing
- Dear ImGui with Vulkan backend (UI rendered through Vulkan)
- Local GeoJSON layers from Open Baltimore
- Local OpenStreetMap raster tiles rendered as Vulkan textures

## Install dependencies (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ python3 libvulkan-dev vulkan-tools libglfw3-dev xorg-dev libwayland-dev
```

## Run

```bash
./run.sh
```

`run.sh` will:
1. Download all layer files from `layers_manifest.json` to `data/layers/`
2. Download local OSM tiles to `data/tiles/`
3. Build the Vulkan app
4. Launch it

## Controls

- Left-click drag on map: pan
- Mouse wheel on map: zoom in/out (11-14), anchored at cursor
- Layer toggles in left panel

## Performance and reliability choices

- Bounded LRU tile cache (`320` textures) with deterministic Vulkan resource destruction
- Reusable Vulkan upload command pool/buffer (no transient command-pool churn per tile)
- Lazy visible-tile requests with background PNG decode worker
- Main-thread upload budget with reusable Vulkan upload command pool/buffer
- Bounded LRU cache eviction to cap GPU memory
- Overlay feature sampling cap per layer for interactive framerate

## Layers (17)

Defined in `layers_manifest.json` and sourced from Open Baltimore official GeoJSON endpoints.

Crime coverage now includes:
- NIBRS Group A Crime Data (2022-present)
- Part 1 Crime Data (Legacy SRS)

## Layer download phases

Use phased manifests to control download volume:

```bash
# compact core set
python3 download_layers.py --phase must-have

# medium add-on set
python3 download_layers.py --phase nice-to-have

# largest/heaviest layers
python3 download_layers.py --phase heavy-data

# nonprofit, public-award, lending, and housing-capital source data
python3 download_layers.py --phase capital-flows

# include nationwide raw 990 XML archives marked large
python3 download_layers.py --phase capital-flows --include-large

# full default set (used by run.sh)
python3 download_layers.py --phase all
```

Phase manifest files:
- `layers_manifest.must_have.json`
- `layers_manifest.nice_to_have.json`
- `layers_manifest.heavy_data.json`
- `layers_manifest.capital_flows.json`
