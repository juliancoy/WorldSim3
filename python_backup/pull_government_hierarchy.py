#!/usr/bin/env python3
import argparse
import json
import pathlib
import re
import urllib.request
import xml.etree.ElementTree as ET
from html.parser import HTMLParser
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "data" / "government"
UA = "WorldSim3/1.0 (+hierarchy-fetcher)"

MD_AGENCY_URL = "https://www.maryland.gov/your-government/state-agencies-and-departments"
BALTIMORE_HOME_URL = "https://www.baltimorecity.gov/"
BALTIMORE_SITEMAP_URL = "https://www.baltimorecity.gov/sitemap.xml?page=1"
OPM_EX_2026_URL = "https://www.opm.gov/policy-data-oversight/pay-leave/salaries-wages/salary-tables/26Tables/exec/html/EX.aspx"
OPM_GS_2026_URL = "https://www.opm.gov/policy-data-oversight/pay-leave/salaries-wages/2026/general-schedule"


def fetch_text(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read().decode("utf-8", errors="replace")


class SectionListParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.in_h2 = False
        self.in_h3 = False
        self.in_li = False
        self.in_a = False
        self.cur_h = ""
        self.cur_li = ""
        self.cur_href = ""
        self.hierarchy: dict[str, list[dict[str, str]]] = {}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attrs_d = dict(attrs)
        if tag in ("h2", "h3"):
            self.cur_h = ""
            self.in_h2 = tag == "h2"
            self.in_h3 = tag == "h3"
        elif tag == "li":
            self.cur_li = ""
            self.cur_href = ""
            self.in_li = True
        elif tag == "a" and self.in_li:
            self.in_a = True
            self.cur_href = attrs_d.get("href") or ""

    def handle_endtag(self, tag: str) -> None:
        if tag in ("h2", "h3"):
            heading = normalize_space(self.cur_h)
            if heading and heading.lower() not in {"on this page", "section menu"}:
                self.hierarchy.setdefault(heading, [])
            self.in_h2 = False
            self.in_h3 = False
        elif tag == "a":
            self.in_a = False
        elif tag == "li":
            text = normalize_space(self.cur_li)
            if text and self.hierarchy:
                last_heading = list(self.hierarchy.keys())[-1]
                self.hierarchy[last_heading].append({"name": text, "url": self.cur_href})
            self.in_li = False

    def handle_data(self, data: str) -> None:
        if self.in_h2 or self.in_h3:
            self.cur_h += data
        elif self.in_li:
            self.cur_li += data


class BaltimoreNavParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.in_li = False
        self.li_classes: list[str] = []
        self.cur_href = ""
        self.cur_text = ""
        self.in_a = False
        self.rows: list[dict[str, Any]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attrs_d = dict(attrs)
        if tag == "li":
            cls = attrs_d.get("class") or ""
            self.li_classes = cls.split()
            self.in_li = "menu-item" in cls
            self.cur_href = ""
            self.cur_text = ""
        elif tag == "a" and self.in_li:
            self.in_a = True
            self.cur_href = attrs_d.get("href") or ""

    def handle_endtag(self, tag: str) -> None:
        if tag == "a":
            self.in_a = False
        elif tag == "li" and self.in_li:
            txt = normalize_space(self.cur_text)
            if txt and self.cur_href:
                level = 0
                for c in self.li_classes:
                    m = re.match(r"menu-item--level-(\\d+)", c)
                    if m:
                        level = int(m.group(1))
                        break
                self.rows.append({"level": level, "name": txt, "url": self.cur_href})
            self.in_li = False

    def handle_data(self, data: str) -> None:
        if self.in_a:
            self.cur_text += data


def normalize_space(s: str) -> str:
    return re.sub(r"\\s+", " ", s).strip()


def parse_md_hierarchy(html: str) -> dict[str, Any]:
    p = SectionListParser()
    p.feed(html)
    keep = {}
    for k, v in p.hierarchy.items():
        if len(v) < 2:
            continue
        if k.lower().startswith("footer"):
            continue
        keep[k] = dedupe_named(v)
    return {
        "source": MD_AGENCY_URL,
        "sections": keep,
    }


def dedupe_named(items: list[dict[str, str]]) -> list[dict[str, str]]:
    out = []
    seen = set()
    for it in items:
        name = normalize_space(it.get("name", ""))
        if not name or name.lower() in seen:
            continue
        seen.add(name.lower())
        out.append({"name": name, "url": it.get("url", "")})
    return out


def parse_baltimore_hierarchy(sitemap_xml: str) -> dict[str, Any]:
    locs: list[str] = []
    try:
        root = ET.fromstring(sitemap_xml)
        ns = {"sm": "http://www.sitemaps.org/schemas/sitemap/0.9"}
        locs = [e.text.strip() for e in root.findall(".//sm:loc", ns) if e.text and "baltimorecity.gov/" in e.text]
    except Exception:
        locs = re.findall(r"<loc>(https://www.baltimorecity.gov/[^<]+)</loc>", sitemap_xml)
    agencies = []
    skip = {
        "news",
        "events",
        "services",
        "departments",
        "government",
        "visitors",
        "residents",
        "business",
    }
    for u in locs:
        path = u.replace("https://www.baltimorecity.gov/", "").strip("/")
        if not path or "/" in path:
            continue
        if path in skip:
            continue
        name = path.replace("-", " ").replace("_", " ").title()
        agencies.append({"name": name, "slug": path, "url": u})
    sections: dict[str, list[dict[str, str]]] = {"City Agencies and Offices": dedupe_named(agencies)}
    return {
        "source": BALTIMORE_SITEMAP_URL,
        "sections": sections,
    }


def parse_ex_pay(html: str) -> dict[str, Any]:
    levels = {}
    for lvl, rate in re.findall(r"Level\\s+([IVX]+)\\s*\\$([0-9,]+)", html):
        levels[f"EX-{lvl}"] = int(rate.replace(",", ""))
    if not levels:
        levels = {
            "EX-I": 253100,
            "EX-II": 228000,
            "EX-III": 209600,
            "EX-IV": 197200,
            "EX-V": 184900,
        }
    return {
        "source": OPM_EX_2026_URL,
        "year": 2026,
        "note": "Fallback values are used if OPM blocks direct parsing in this runtime.",
        "levels": levels,
    }


def parse_gs_pay_links(html: str) -> dict[str, Any]:
    links = re.findall(r'href="([^"]+?\\.xml)"', html, flags=re.IGNORECASE)
    norm_links = []
    for l in links:
        if l.startswith("/"):
            l = "https://www.opm.gov" + l
        norm_links.append(l)
    norm_links = sorted(set(norm_links))
    return {
        "source": OPM_GS_2026_URL,
        "year": 2026,
        "xml_tables": norm_links,
        "table_count": len(norm_links),
        "note": "If links are empty, fetch from the OPM page in a browser and use XML Data links directly.",
    }


def federal_salient_positions() -> list[dict[str, Any]]:
    return [
        {"position": "President", "branch": "Executive", "pay_reference": "Statutory (3 U.S.C. 102)", "pay_schedule": "Not OPM GS/EX"},
        {"position": "Vice President", "branch": "Executive/Legislative", "pay_reference": "Executive Schedule Level II (official), frozen payable noted by OPM in Jan 2026", "pay_schedule": "EX-II"},
        {"position": "Cabinet Secretaries", "branch": "Executive", "pay_reference": "5 U.S.C. 5313", "pay_schedule": "EX-I"},
        {"position": "Deputy Secretaries", "branch": "Executive", "pay_reference": "5 U.S.C. 5314", "pay_schedule": "EX-II"},
        {"position": "Under Secretaries / Administrator-tier", "branch": "Executive", "pay_reference": "5 U.S.C. 5315", "pay_schedule": "EX-III"},
        {"position": "Assistant Secretaries / many agency heads", "branch": "Executive", "pay_reference": "5 U.S.C. 5315-5316", "pay_schedule": "EX-IV/EX-V"},
        {"position": "Senior Executive Service", "branch": "Executive", "pay_reference": "OPM SES compensation guidance", "pay_schedule": "SES range capped by EX-III or EX-II depending on certification"},
        {"position": "Most federal civil service roles", "branch": "Executive", "pay_reference": "5 U.S.C. ch. 53", "pay_schedule": "GS + locality / special rates"},
        {"position": "U.S. Senators and Representatives", "branch": "Legislative", "pay_reference": "2 U.S.C. 4501", "pay_schedule": "Statutory congressional salary"},
        {"position": "Chief Justice / Associate Justices", "branch": "Judicial", "pay_reference": "28 U.S.C. 5, 44", "pay_schedule": "Judiciary statutory salary"},
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description="Pull Maryland, Baltimore, and federal hierarchy + pay schedule references")
    parser.add_argument("--out-dir", default=str(OUT_DIR), help="Output directory")
    args = parser.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    md_html = fetch_text(MD_AGENCY_URL)
    balt_sitemap_xml = fetch_text(BALTIMORE_SITEMAP_URL)
    ex_html = fetch_text(OPM_EX_2026_URL)
    gs_html = fetch_text(OPM_GS_2026_URL)

    payload = {
        "generated_at": __import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat(),
        "maryland": parse_md_hierarchy(md_html),
        "baltimore": parse_baltimore_hierarchy(balt_sitemap_xml),
        "federal": {
            "salient_positions": federal_salient_positions(),
            "pay_schedules": {
                "executive_schedule": parse_ex_pay(ex_html),
                "general_schedule": parse_gs_pay_links(gs_html),
            },
            "sources": [
                "https://www.opm.gov/policy-data-oversight/pay-leave/salaries-wages/salary-tables/",
                "https://www.opm.gov/policy-data-oversight/senior-executive-service/compensation/",
            ],
        },
    }

    out_path = out_dir / "government_hierarchy_and_pay_2026.json"
    out_path.write_text(json.dumps(payload, indent=2))
    print(f"Wrote {out_path}")
    print(f"Maryland sections: {len(payload['maryland']['sections'])}")
    print(f"Baltimore sections: {len(payload['baltimore']['sections'])}")
    print(f"EX levels captured: {len(payload['federal']['pay_schedules']['executive_schedule']['levels'])}")
    print(f"GS XML tables linked: {payload['federal']['pay_schedules']['general_schedule']['table_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
