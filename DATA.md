# Data Acquisition Guide

## Dataset versioning and diff updates

On-demand dataset downloads use conditional HTTP and local version artifacts:

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

Native preload/downloads do not require Python:

```bash
# Download the compact core set and exit
./build/worldsim3 --download-layers must-have

# Download the full default manifest and exit
./build/worldsim3 --download-layers all

# Download before launching the app
WORLD_SIM3_PRELOAD_DATA=1 WORLD_SIM3_PRELOAD_PHASE=must-have ./build/worldsim3
```

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
