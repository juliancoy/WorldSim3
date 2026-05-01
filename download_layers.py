#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "scripts" / "layers_manifest.json"
DATA_DIR = ROOT / "data" / "layers"
PUBLIC_MANIFEST = ROOT / "web" / "layers_manifest.json"

PHASE_MANIFESTS = {
    "all": ROOT / "scripts" / "layers_manifest.json",
    "must-have": ROOT / "scripts" / "layers_manifest.must_have.json",
    "nice-to-have": ROOT / "scripts" / "layers_manifest.nice_to_have.json",
    "heavy-data": ROOT / "scripts" / "layers_manifest.heavy_data.json",
}


def download(url: str, path: pathlib.Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=120) as response:
        path.write_bytes(response.read())


def resolve_manifest(manifest_arg: str | None, phase: str | None) -> pathlib.Path:
    if manifest_arg:
        manifest_path = pathlib.Path(manifest_arg)
        if not manifest_path.is_absolute():
            manifest_path = ROOT / manifest_path
        return manifest_path

    if phase:
        return PHASE_MANIFESTS[phase]

    return DEFAULT_MANIFEST


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=str, help="Path to manifest JSON file")
    parser.add_argument("--phase", choices=sorted(PHASE_MANIFESTS.keys()), help="Named manifest phase")
    args = parser.parse_args()

    manifest_path = resolve_manifest(args.manifest, args.phase)
    if not manifest_path.exists():
        print(f"manifest not found: {manifest_path}")
        return 1

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    layers = json.loads(manifest_path.read_text())

    failures = []
    print(f"Using manifest: {manifest_path}")
    for i, layer in enumerate(layers, start=1):
        out_path = DATA_DIR / layer["file"]
        print(f"[{i}/{len(layers)}] downloading {layer['name']} -> {out_path.name}")
        try:
            download(layer["url"], out_path)
        except Exception as exc:  # noqa: BLE001
            failures.append((layer["name"], str(exc)))
            print(f"  failed: {exc}")

    if manifest_path == DEFAULT_MANIFEST:
        PUBLIC_MANIFEST.write_text(json.dumps(layers, indent=2))
        print(f"\nWrote web manifest: {PUBLIC_MANIFEST}")

    if failures:
        print("\nSome layers failed:")
        for name, err in failures:
            print(f"- {name}: {err}")
        return 1

    print("\nAll layers downloaded.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
