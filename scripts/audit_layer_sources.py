#!/usr/bin/env python3
"""Validate source URLs in worldsim layer manifests.

The app can download layers from top-level `url` entries and from selected
`import` sources. `source_urls` and `reference_url` are provenance links; they
are still checked, but failures there are reported as source metadata issues
rather than app download failures.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
from pathlib import Path


def manifest_layers(path: Path) -> list[dict]:
    data = json.loads(path.read_text())
    if isinstance(data, list):
        return data
    if isinstance(data, dict) and isinstance(data.get("layers"), list):
        return data["layers"]
    raise ValueError(f"{path}: expected a layer list or object with layers[]")


def collect_urls(paths: list[Path]) -> dict[str, list[tuple[str, int, str, str, bool]]]:
    contexts: dict[str, list[tuple[str, int, str, str, bool]]] = {}
    for path in paths:
        for idx, layer in enumerate(manifest_layers(path), start=1):
            name = str(layer.get("name", "?"))
            download_enabled = layer.get("download", True) is not False
            if layer.get("url"):
                kind = "download.url" if download_enabled else "reference.url"
                contexts.setdefault(layer["url"], []).append((str(path), idx, name, kind, download_enabled))
            import_def = layer.get("import") if isinstance(layer.get("import"), dict) else {}
            for key in ("url", "service_url"):
                if import_def.get(key):
                    contexts.setdefault(import_def[key], []).append((str(path), idx, name, f"import.{key}", download_enabled))
            if layer.get("reference_url"):
                contexts.setdefault(layer["reference_url"], []).append((str(path), idx, name, "reference_url", False))
            for source_url in layer.get("source_urls") or []:
                if isinstance(source_url, str):
                    contexts.setdefault(source_url, []).append((str(path), idx, name, "source_urls", False))
    return contexts


def curl_check(url: str, timeout_seconds: int) -> tuple[str, str, str, str]:
    def run(method: str) -> tuple[int, str, str]:
        cmd = [
            "curl",
            "-L",
            "--max-time",
            str(timeout_seconds),
            "--connect-timeout",
            "8",
            "-A",
            "worldsim3-source-audit/1.0",
            "-o",
            "/dev/null",
            "-sS",
            "-w",
            "%{http_code}\t%{content_type}\t%{url_effective}",
        ]
        if method == "HEAD":
            cmd.append("-I")
        else:
            cmd += ["--range", "0-0"]
        cmd.append(url)
        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout_seconds + 5,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return 124, "", "timeout"
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()

    rc, out, err = run("HEAD")
    code = out.split("\t", 1)[0] if out else "000"
    if rc != 0 or code in {"000", "403", "405"}:
        rc, out, err = run("GET")
        code = out.split("\t", 1)[0] if out else "000"
    return url, code, out, err


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifests", nargs="*", default=["layers_manifest.json"])
    parser.add_argument("--all", action="store_true", help="audit all layers_manifest*.json files")
    parser.add_argument("--workers", type=int, default=12)
    parser.add_argument("--timeout", type=int, default=20)
    args = parser.parse_args()

    paths = sorted(Path(".").glob("layers_manifest*.json")) if args.all else [Path(p) for p in args.manifests]
    contexts = collect_urls(paths)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        results = list(executor.map(lambda url: curl_check(url, args.timeout), contexts))

    failures = []
    blocked_reference_urls = []
    for url, code, out, err in results:
        if not code or code[0] not in {"2", "3"}:
            refs = contexts[url]
            only_reference = all(not download_enabled for *_, download_enabled in refs)
            if code == "403" and only_reference:
                blocked_reference_urls.append((url, code, out, err, refs))
            else:
                failures.append((url, code, out, err, refs))

    print(
        f"manifests={len(paths)} unique_urls={len(contexts)} "
        f"failures={len(failures)} blocked_references={len(blocked_reference_urls)}"
    )
    for url, code, out, err, refs in failures:
        print(f"\nFAIL {code} {url}")
        if out:
            print(f"  curl: {out[:240]}")
        if err:
            print(f"  err: {err[:240]}")
        for path, idx, name, kind, _download_enabled in refs[:8]:
            print(f"  {path}:{idx} {kind}: {name}")
    for url, code, out, err, refs in blocked_reference_urls:
        print(f"\nBLOCKED_REFERENCE {code} {url}")
        if out:
            print(f"  curl: {out[:240]}")
        if err:
            print(f"  err: {err[:240]}")
        for path, idx, name, kind, _download_enabled in refs[:8]:
            print(f"  {path}:{idx} {kind}: {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
