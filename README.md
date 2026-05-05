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
1. Build the Vulkan app
2. Launch it

The app no longer auto-downloads all data at startup. Use the in-app `Library` window and per-layer `D` button to download missing datasets on demand.

LAN sharing and peer signaling are available while the app runs:
- Status API (local only): `http://127.0.0.1:8787/status`
- Dataset API (LAN): `http://<host-ip>:8788/datasets`
- File fetch (LAN): `http://<host-ip>:8788/dataset/file?path=data/layers/<file>.geojson`
- P2P signaling (LAN): `http://<host-ip>:8788/p2p/register`, `/p2p/publish`, `/p2p/poll`
- LAN discovery/version check (UDP): broadcast probe `WS3_DISCOVER_V1` on port `8789`

The app includes a `Scan LAN Peers` button that checks peer `protocol_version` compatibility before use.

## Dataset versioning and diff updates

On-demand dataset downloads now use conditional HTTP and local version artifacts:

- Conditional fetch headers: `If-None-Match` (ETag), `If-Modified-Since`
- Metadata per dataset: `data/versions/metadata/<file>.json`
- Immutable snapshots: `data/versions/snapshots/<file>/...`
- GeoJSON feature diffs (added/removed/changed): `data/versions/diffs/<file>/...`

When source returns `304 Not Modified`, the local file is left untouched and only `checked_at` is updated in metadata.

Data Library workflow:
- `Check` (per dataset) performs non-mutating freshness checks.
- `Check All Updates` checks all downloaded datasets with source URLs.
- `Update` button appears only for rows flagged `Update available`.

Optional preload mode:

```bash
WORLD_SIM3_PRELOAD_DATA=1 ./run.sh
```

## Controls

- Left-click drag on map: pan
- Mouse wheel on map: zoom in/out (11-14), anchored at cursor
- Layer toggles in left panel

## Performance and reliability choices

- Hardware note: this runs reasonably well for Baltimore-scale data on an NVIDIA RTX 3060.
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

## HMDA mortgage layer

Generate the Baltimore tract-level HMDA mortgage layer (2024) from the FFIEC/CFPB HMDA API:

```bash
python3 generate_hmda_baltimore_tracts.py
```

This writes `data/layers/hmda_mortgage_2024_baltimore_tracts.geojson`.

## Alternative simplified model views

Generate simplified representations for all local GeoJSON datasets (compact and detailed views):

```bash
python3 generate_simplified_views.py
```

Outputs:
- `data/models/simplified_views.compact.json`
- `data/models/simplified_views.detailed.json`

## Government hierarchy + federal pay schedule pull

Pull Maryland and Baltimore government hierarchies plus salient federal positions and pay-schedule references:

```bash
python3 pull_government_hierarchy.py
```

Output:
- `data/government/government_hierarchy_and_pay_2026.json`

## Additional Representation Data + Power Statistics

Pull the extended domain catalog and Maryland power use/generation statistics:

```bash
python3 pull_additional_representations_data.py
```

Outputs:
- `data/representations/additional_data_catalog.json`
- `data/energy/maryland_power_use_generation_2024.json`

## LAN Dataset Serving + P2P Signaling

Serve all local datasets to your LAN with search APIs and a lightweight peer-signaling layer:

```bash
python3 serve_datasets_p2p.py --host 0.0.0.0 --port 8788
```

Key endpoints:
- `GET /api/datasets?q=<search>`
- `GET /api/file?path=data/layers/<file>`
- `GET /api/refresh`
- `POST /api/p2p/register`
- `POST /api/p2p/send`
- `GET /api/p2p/poll?peer_id=<id>`

Cloudflare Worker signaling template:
- `cloudflare/worker-signaling.js`

Note: signaling enables peers to exchange WebRTC ICE/SDP offers; NAT traversal itself depends on STUN/TURN from the clients.

## Arkavo WebRTC Client (TypeScript)

Drop-in browser client module:
- `web/arkavo/realtime-client.ts`
- `web/arkavo/demo.ts`
- `web/arkavo/index.html`

Defaults baked into the module:
- Signaling: `wss://signaling.arkavo.org/`
- TURN: from server `hello.ice.turn` with `hello.ice.username` + `hello.ice.credential`
- STUN fallback: `stun:stun.l.google.com:19302` when no STUN is provided

Build demo:

```bash
tsc web/arkavo/realtime-client.ts web/arkavo/demo.ts \
  --target ES2021 --module ES2020 --lib DOM,ES2021 \
  --outDir web/arkavo/dist
```

Then open `web/arkavo/index.html` in two browsers, join the same room, and exchange files.

## Arkavo Native C++ Integration

Native module files:
- `arkavo_realtime_client.h`
- `arkavo_realtime_client.cpp`
- `arkavo_signaling_transport_curl.h`
- `arkavo_signaling_transport_curl.cpp`
- `arkavo_rtc_session_manager.h`
- `arkavo_rtc_session_manager.cpp`

What is implemented:
- Arkavo signaling protocol parser/validator (`hello`, `joined`, `peer-joined`, `peer-left`, `signal`)
- Room join flow
- Peer session tracking
- Exponential reconnect scheduling
- Safe message-shape validation before acting
- Native `wss://` signaling transport using libcurl WebSocket APIs
- Native WebRTC peer connections and data channels via `libdatachannel`
- Chunked file transfer over RTCDataChannel

Standalone connectivity test:

```bash
cmake --build build --target arkavo_connectivity_test -j4
./build/arkavo_connectivity_test --room worldsim-test-a --timeout 120
```

Run the same command on a second machine or terminal with the same room. To test file transfer after a peer ID appears:

```bash
./build/arkavo_connectivity_test --room worldsim-test-a --send-peer <peer-id> --send-file data/models/simplified_views.compact.json
```
