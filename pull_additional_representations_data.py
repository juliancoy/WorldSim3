#!/usr/bin/env python3
import json
import pathlib
import re
import urllib.request
from datetime import datetime, timezone
from html.parser import HTMLParser
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent
OUT_DIR = ROOT / "data" / "representations"
ENERGY_DIR = ROOT / "data" / "energy"
UA = "WorldSim3/1.0 (+additional-representations)"

EIA_MD_PROFILE_URL = "https://www.eia.gov/electricity/state/maryland/"


class SimpleTableParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.in_table = False
        self.in_row = False
        self.in_cell = False
        self.current_cell = ""
        self.current_row: list[str] = []
        self.rows: list[list[str]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attrs_d = dict(attrs)
        if tag == "table" and "basic-table" in (attrs_d.get("class") or ""):
            self.in_table = True
        elif self.in_table and tag == "tr":
            self.in_row = True
            self.current_row = []
        elif self.in_row and tag in ("th", "td"):
            self.in_cell = True
            self.current_cell = ""

    def handle_endtag(self, tag: str) -> None:
        if tag == "table" and self.in_table:
            self.in_table = False
        elif tag == "tr" and self.in_row:
            if self.current_row:
                self.rows.append(self.current_row)
            self.in_row = False
        elif tag in ("th", "td") and self.in_cell:
            txt = re.sub(r"\s+", " ", self.current_cell).strip()
            self.current_row.append(txt)
            self.in_cell = False

    def handle_data(self, data: str) -> None:
        if self.in_cell:
            self.current_cell += data


def fetch_text(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read().decode("utf-8", errors="replace")


def parse_eia_md_summary(html: str) -> dict[str, Any]:
    p = SimpleTableParser()
    p.feed(html)
    rows = p.rows

    table_title = ""
    m = re.search(r"Table\s*1\.\s*2024\s*Summary statistics \(Maryland\)", html, flags=re.IGNORECASE)
    if m:
        table_title = "Table 1. 2024 Summary statistics (Maryland)"

    # Keep first substantial table block (the summary table on this page).
    filtered_rows = [r for r in rows if len(r) >= 2]

    key_metrics = {}
    for r in filtered_rows:
        k = r[0].strip().lower()
        if "net generation" in k:
            key_metrics["net_generation"] = r
        elif "total retail sales" in k:
            key_metrics["total_retail_sales"] = r
        elif "average retail price" in k:
            key_metrics["average_retail_price"] = r
        elif "net summer capacity" in k:
            key_metrics["net_summer_capacity"] = r

    return {
        "source": EIA_MD_PROFILE_URL,
        "table_title": table_title,
        "profile_year": 2024,
        "rows": filtered_rows,
        "salient_metrics": key_metrics,
    }


def build_catalog() -> dict[str, Any]:
    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "domains": [
            {
                "domain": "parcel_transactions",
                "priority": "high",
                "datasets": [
                    "sales history",
                    "permits",
                    "code enforcement outcomes",
                    "foreclosure filings",
                ],
                "status": "planned",
            },
            {
                "domain": "housing_market",
                "priority": "high",
                "datasets": ["rent estimates", "eviction filings/judgments", "subsidized housing inventory", "waitlist pressure"],
                "status": "planned",
            },
            {
                "domain": "mobility_access",
                "priority": "high",
                "datasets": ["GTFS schedules", "reliability", "commute flows", "travel-time-to-services"],
                "status": "planned",
            },
            {
                "domain": "health_utilization",
                "priority": "medium",
                "datasets": ["ED visits", "avoidable hospitalizations", "behavioral health capacity"],
                "status": "planned",
            },
            {
                "domain": "education_youth",
                "priority": "medium",
                "datasets": ["attendance", "chronic absenteeism", "graduation", "youth program participation"],
                "status": "planned",
            },
            {
                "domain": "economic_resilience",
                "priority": "high",
                "datasets": ["unemployment claims", "business openings/closures", "wages by sector"],
                "status": "planned",
            },
            {
                "domain": "environmental_burden",
                "priority": "high",
                "datasets": ["heat islands", "flood risk", "air quality", "lead risk", "tree canopy"],
                "status": "planned",
            },
            {
                "domain": "public_safety_detail",
                "priority": "high",
                "datasets": ["calls-for-service timelines", "clearance rates", "victim/offender demographics"],
                "status": "planned",
            },
            {
                "domain": "fiscal_flows",
                "priority": "high",
                "datasets": ["city/state budget lines", "procurement awards", "grant disbursement timing"],
                "status": "planned",
            },
            {
                "domain": "service_delivery_ops",
                "priority": "high",
                "datasets": ["311 lifecycle", "repeat requests", "backlog"],
                "status": "planned",
            },
            {
                "domain": "governance_staffing",
                "priority": "medium",
                "datasets": ["filled vs vacant positions", "attrition", "hiring pipeline"],
                "status": "planned",
            },
            {
                "domain": "ground_truth_quality",
                "priority": "high",
                "datasets": ["field audits", "survey validation", "map error reports"],
                "status": "planned",
            },
            {
                "domain": "power_use_generation",
                "priority": "high",
                "datasets": ["state net generation", "retail sales/consumption", "capacity", "retail price"],
                "status": "pulled",
                "source": EIA_MD_PROFILE_URL,
            },
        ],
        "source_references": [
            "https://www.eia.gov/electricity/state/maryland/",
            "https://www.baltimorecity.gov/",
            "https://www.maryland.gov/",
            "https://www.census.gov/data/developers/data-sets.html",
            "https://www.bls.gov/developers/",
            "https://api.census.gov/data/timeseries/",
        ],
    }


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ENERGY_DIR.mkdir(parents=True, exist_ok=True)

    catalog = build_catalog()
    (OUT_DIR / "additional_data_catalog.json").write_text(json.dumps(catalog, indent=2))

    eia_html = fetch_text(EIA_MD_PROFILE_URL)
    eia_data = parse_eia_md_summary(eia_html)
    (ENERGY_DIR / "maryland_power_use_generation_2024.json").write_text(json.dumps(eia_data, indent=2))

    print(f"Wrote {OUT_DIR / 'additional_data_catalog.json'}")
    print(f"Wrote {ENERGY_DIR / 'maryland_power_use_generation_2024.json'}")
    print(f"Power table rows: {len(eia_data['rows'])}")
    print(f"Salient metrics captured: {len(eia_data['salient_metrics'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
