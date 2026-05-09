# Homelessness Layers: Official Download + Drag-and-Drop Ingest (C++)

## Official HUD PIT source (CoC trend table)
Use HUD's official AHAR 2024 resource page:

- https://www.huduser.gov/portal/datasets/ahar/2024-ahar-part-1-pit-estimates-of-homelessness-in-the-us.html

Download this file from Resource Links:

- `2007 - 2024 Point-in-Time Estimates by CoC (XLSB)`

## Drag-and-drop location
Export that workbook to CSV (save as CSV) and drag-and-drop the CSV into:

- `data/inbox/hud_pit/`

Supported ingest format:
- `.csv`

## Build the map layer
Build the tool once:

```bash
cmake --build build -j4 --target worldsim_hud_pit_builder
```

Run:

```bash
./build/worldsim_hud_pit_builder
```

The script auto-picks the newest file in `data/inbox/hud_pit/`.

Output:

- `data/layers/hud_pit_homelessness_2007_2024_by_coc.geojson`

## Explicit file path (optional)

```bash
./build/worldsim_hud_pit_builder \
  --pit-csv /absolute/path/to/2007-2024-PIT-Counts-by-CoC.csv
```
