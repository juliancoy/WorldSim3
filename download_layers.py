#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
DEFAULT_MANIFEST = ROOT / "layers_manifest.json"
DATA_DIR = ROOT / "data" / "layers"
CAPITAL_FLOWS_DIR = ROOT / "data" / "capital_flows"
PUBLIC_MANIFEST = ROOT / "web" / "layers_manifest.json"

PHASE_MANIFESTS = {
    "all": ROOT / "layers_manifest.json",
    "must-have": ROOT / "layers_manifest.must_have.json",
    "nice-to-have": ROOT / "layers_manifest.nice_to_have.json",
    "heavy-data": ROOT / "layers_manifest.heavy_data.json",
    "capital-flows": ROOT / "layers_manifest.capital_flows.json",
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


def output_dir_for(item: dict) -> pathlib.Path:
    if "directory" in item:
        path = pathlib.Path(item["directory"])
        return path if path.is_absolute() else ROOT / path
    if item.get("category") == "capital-flows":
        return CAPITAL_FLOWS_DIR
    return DATA_DIR


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=str, help="Path to manifest JSON file")
    parser.add_argument("--phase", choices=sorted(PHASE_MANIFESTS.keys()), help="Named manifest phase")
    parser.add_argument("--include-large", action="store_true", help="Download entries marked large")
    args = parser.parse_args()

    manifest_path = resolve_manifest(args.manifest, args.phase)
    if not manifest_path.exists():
        print(f"manifest not found: {manifest_path}")
        return 1

    items = json.loads(manifest_path.read_text())

    failures = []
    skipped = []
    print(f"Using manifest: {manifest_path}")
    for i, item in enumerate(items, start=1):
        name = item["name"]
        if item.get("download") is False:
            skipped.append((name, item.get("reason", "metadata/API/manual source")))
            print(f"[{i}/{len(items)}] skipping {name}: {skipped[-1][1]}")
            continue
        if item.get("large") and not args.include_large:
            skipped.append((name, "large source; rerun with --include-large"))
            print(f"[{i}/{len(items)}] skipping {name}: {skipped[-1][1]}")
            continue
        if "url" not in item:
            failures.append((name, "missing url"))
            print(f"[{i}/{len(items)}] failed {name}: missing url")
            continue

        out_dir = output_dir_for(item)
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / item["file"]
        print(f"[{i}/{len(items)}] downloading {name} -> {out_path}")
        try:
            download(item["url"], out_path)
        except Exception as exc:  # noqa: BLE001
            failures.append((name, str(exc)))
            print(f"  failed: {exc}")

    if manifest_path == DEFAULT_MANIFEST and PUBLIC_MANIFEST.parent.exists():
        PUBLIC_MANIFEST.write_text(json.dumps(items, indent=2))
        print(f"\nWrote web manifest: {PUBLIC_MANIFEST}")

    if skipped:
        print("\nSkipped sources:")
        for name, reason in skipped:
            print(f"- {name}: {reason}")

    if failures:
        print("\nSome layers failed:")
        for name, err in failures:
            print(f"- {name}: {err}")
        return 1

    print("\nAll requested downloads completed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
