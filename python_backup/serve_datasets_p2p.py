#!/usr/bin/env python3
import argparse
import json
import mimetypes
import pathlib
import threading
import time
import uuid
from collections import defaultdict, deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_DATA_DIRS = [
    ROOT / "data" / "layers",
    ROOT / "data" / "capital_flows",
    ROOT / "data" / "government",
    ROOT / "data" / "models",
    ROOT / "data" / "representations",
    ROOT / "data" / "energy",
]


class DatasetIndex:
    def __init__(self, roots: list[pathlib.Path]) -> None:
        self.roots = roots
        self._lock = threading.Lock()
        self._datasets = []
        self.refresh()

    def refresh(self) -> None:
        rows = []
        for base in self.roots:
            if not base.exists():
                continue
            for p in sorted(base.rglob("*")):
                if not p.is_file():
                    continue
                rel = p.relative_to(ROOT).as_posix()
                rows.append({
                    "id": rel,
                    "path": rel,
                    "name": p.name,
                    "category": str(base.relative_to(ROOT)).replace("data/", ""),
                    "size_bytes": p.stat().st_size,
                    "mtime_unix": int(p.stat().st_mtime),
                })
        with self._lock:
            self._datasets = rows

    def list(self, q: str = "") -> list[dict]:
        qn = q.strip().lower()
        with self._lock:
            if not qn:
                return list(self._datasets)
            out = []
            for d in self._datasets:
                hay = f"{d['name']} {d['path']} {d['category']}".lower()
                if qn in hay:
                    out.append(d)
            return out


class P2PSignaling:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._mailboxes: dict[str, deque] = defaultdict(deque)

    def register(self) -> str:
        pid = str(uuid.uuid4())
        with self._lock:
            self._mailboxes[pid]
        return pid

    def send(self, sender: str, recipient: str, payload: dict) -> bool:
        msg = {
            "id": str(uuid.uuid4()),
            "sender": sender,
            "recipient": recipient,
            "payload": payload,
            "ts": time.time(),
        }
        with self._lock:
            if recipient not in self._mailboxes:
                return False
            self._mailboxes[recipient].append(msg)
        return True

    def poll(self, peer_id: str, max_items: int = 64) -> list[dict]:
        with self._lock:
            if peer_id not in self._mailboxes:
                return []
            box = self._mailboxes[peer_id]
            out = []
            for _ in range(min(max_items, len(box))):
                out.append(box.popleft())
            return out


class App:
    def __init__(self, index: DatasetIndex, signaling: P2PSignaling) -> None:
        self.index = index
        self.signaling = signaling


class Handler(BaseHTTPRequestHandler):
    server_version = "WorldSim3DatasetP2P/1.0"

    @property
    def app(self) -> App:
        return self.server.app  # type: ignore[attr-defined]

    def _json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.end_headers()

    def do_GET(self) -> None:
        u = urlparse(self.path)
        if u.path == "/health":
            self._json({"ok": True, "service": "dataset-p2p"})
            return
        if u.path == "/api/datasets":
            qs = parse_qs(u.query)
            q = qs.get("q", [""])[0]
            items = self.app.index.list(q)
            self._json({"ok": True, "count": len(items), "datasets": items})
            return
        if u.path == "/api/refresh":
            self.app.index.refresh()
            self._json({"ok": True, "message": "dataset index refreshed"})
            return
        if u.path == "/api/file":
            qs = parse_qs(u.query)
            rel = qs.get("path", [""])[0]
            self._serve_file(rel)
            return
        if u.path == "/api/p2p/poll":
            qs = parse_qs(u.query)
            peer_id = qs.get("peer_id", [""])[0]
            max_items = int(qs.get("max", ["64"])[0])
            msgs = self.app.signaling.poll(peer_id, max_items=max_items)
            self._json({"ok": True, "peer_id": peer_id, "messages": msgs})
            return

        self._json({"ok": False, "error": "not found"}, status=404)

    def do_POST(self) -> None:
        u = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception:
            self._json({"ok": False, "error": "invalid json"}, status=400)
            return

        if u.path == "/api/p2p/register":
            peer_id = self.app.signaling.register()
            self._json({"ok": True, "peer_id": peer_id})
            return
        if u.path == "/api/p2p/send":
            sender = str(payload.get("sender", ""))
            recipient = str(payload.get("recipient", ""))
            msg = payload.get("payload", {})
            if not sender or not recipient:
                self._json({"ok": False, "error": "sender and recipient required"}, status=400)
                return
            ok = self.app.signaling.send(sender, recipient, msg)
            if not ok:
                self._json({"ok": False, "error": "recipient not found"}, status=404)
                return
            self._json({"ok": True})
            return

        self._json({"ok": False, "error": "not found"}, status=404)

    def _serve_file(self, rel_path: str) -> None:
        rel_path = unquote(rel_path).lstrip("/")
        full = (ROOT / rel_path).resolve()
        try:
            full.relative_to(ROOT)
        except ValueError:
            self._json({"ok": False, "error": "path outside workspace"}, status=400)
            return
        if not full.exists() or not full.is_file():
            self._json({"ok": False, "error": "file not found"}, status=404)
            return

        ctype, _ = mimetypes.guess_type(str(full))
        ctype = ctype or "application/octet-stream"
        data = full.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve local datasets over LAN with simple P2P signaling")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host (use 0.0.0.0 for LAN)")
    parser.add_argument("--port", type=int, default=8788, help="Bind port")
    args = parser.parse_args()

    index = DatasetIndex(DEFAULT_DATA_DIRS)
    signaling = P2PSignaling()
    app = App(index=index, signaling=signaling)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.app = app  # type: ignore[attr-defined]

    print(f"Serving datasets on http://{args.host}:{args.port}")
    print("Endpoints:")
    print("  GET  /api/datasets?q=<search>")
    print("  GET  /api/file?path=data/layers/<file>")
    print("  GET  /api/refresh")
    print("  POST /api/p2p/register")
    print("  POST /api/p2p/send")
    print("  GET  /api/p2p/poll?peer_id=<id>")
    server.serve_forever()


if __name__ == "__main__":
    raise SystemExit(main())
