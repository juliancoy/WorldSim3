#!/usr/bin/env python3
import csv
import json
import re
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "data" / "public_servants"
RAW = OUT / "raw"
DOCS = OUT / "source_docs"
NORM_JSONL = OUT / "normalized_public_servants.jsonl"
NORM_CSV = OUT / "normalized_public_servants.csv"
CATALOG = OUT / "source_catalog.json"

UA = "WorldSim3 public-sector roster fetcher/0.1 (+local research; contact user)"


def fetch_text(url: str, timeout: int = 60) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def fetch_bytes(url: str, timeout: int = 120) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, sort_keys=True), encoding="utf-8")


def arcgis_query_all(service_url: str, out_fields: str = "*", page_size: int = 2000):
    offset = 0
    while True:
        params = {
            "where": "1=1",
            "outFields": out_fields,
            "f": "json",
            "resultOffset": str(offset),
            "resultRecordCount": str(page_size),
            "orderByFields": "ID ASC",
            "returnGeometry": "false",
        }
        url = service_url.rstrip("/") + "/query?" + urllib.parse.urlencode(params)
        data = json.loads(fetch_text(url))
        if "error" in data:
            raise RuntimeError(data["error"])
        features = data.get("features", [])
        if not features:
            break
        yield from (f.get("attributes", {}) for f in features)
        offset += len(features)
        if len(features) < page_size or not data.get("exceededTransferLimit"):
            break
        time.sleep(0.05)


def norm_money(v):
    if v is None or v == "":
        return ""
    try:
        return f"{float(v):.2f}"
    except Exception:
        return str(v)


def write_normalized(rows):
    fields = [
        "source_id",
        "source_type",
        "jurisdiction",
        "employer",
        "agency",
        "person_name",
        "role_title",
        "pay_grade",
        "annual_salary",
        "gross_pay",
        "fiscal_year",
        "source_url",
        "provenance_note",
    ]
    OUT.mkdir(parents=True, exist_ok=True)
    with NORM_JSONL.open("w", encoding="utf-8") as jf, NORM_CSV.open("w", newline="", encoding="utf-8") as cf:
        writer = csv.DictWriter(cf, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            clean = {k: row.get(k, "") for k in fields}
            jf.write(json.dumps(clean, ensure_ascii=False) + "\n")
            writer.writerow(clean)


def download_baltimore_city(rows, catalog):
    item_url = "https://www.arcgis.com/sharing/rest/content/items/0bceed42ed994e65bec410bdf9c383c8?f=json"
    item = json.loads(fetch_text(item_url))
    service = item["url"].rstrip("/") + "/0"
    RAW.mkdir(parents=True, exist_ok=True)
    write_json(RAW / "baltimore_city_employee_salaries_item.json", item)
    count = 0
    raw_path = RAW / "baltimore_city_employee_salaries.jsonl"
    with raw_path.open("w", encoding="utf-8") as raw:
        for attrs in arcgis_query_all(service):
            raw.write(json.dumps(attrs, ensure_ascii=False) + "\n")
            rows.append({
                "source_id": "baltimore_city_employee_salaries",
                "source_type": "official_bulk_employee_salary",
                "jurisdiction": "Baltimore City",
                "employer": "Baltimore City",
                "agency": attrs.get("AgencyName", ""),
                "person_name": attrs.get("Name", ""),
                "role_title": attrs.get("JobTitle", ""),
                "pay_grade": "",
                "annual_salary": norm_money(attrs.get("AnnualSalary")),
                "gross_pay": norm_money(attrs.get("GrossPay")),
                "fiscal_year": attrs.get("FiscalYear", ""),
                "source_url": service,
                "provenance_note": "Official Baltimore City ArcGIS/Open Data employee salary table.",
            })
            count += 1
    catalog.append({
        "id": "baltimore_city_employee_salaries",
        "name": item.get("title", "Baltimore City Employee Salaries"),
        "jurisdiction": "Baltimore City",
        "kind": "official_bulk_employee_salary",
        "status": "downloaded",
        "records": count,
        "url": service,
        "description": re.sub("<[^>]+>", " ", item.get("description", "")),
    })


def socrata_query_all(domain: str, dataset_id: str, page_size: int = 50000):
    offset = 0
    while True:
        params = {"$limit": str(page_size), "$offset": str(offset)}
        url = f"https://{domain}/resource/{dataset_id}.json?" + urllib.parse.urlencode(params)
        data = json.loads(fetch_text(url))
        if not data:
            break
        yield from data
        offset += len(data)
        if len(data) < page_size:
            break
        time.sleep(0.05)


def download_montgomery_county(rows, catalog):
    domain = "data.montgomerycountymd.gov"
    dataset_id = "2nq6-auk8"
    meta_url = f"https://{domain}/api/views/{dataset_id}"
    data_url = f"https://{domain}/resource/{dataset_id}.json"
    meta = json.loads(fetch_text(meta_url))
    write_json(RAW / "montgomery_county_employee_salaries_2024_metadata.json", meta)
    count = 0
    raw_path = RAW / "montgomery_county_employee_salaries_2024.jsonl"
    with raw_path.open("w", encoding="utf-8") as raw:
        for row in socrata_query_all(domain, dataset_id):
            raw.write(json.dumps(row, ensure_ascii=False) + "\n")
            rows.append({
                "source_id": "montgomery_county_employee_salaries_2024",
                "source_type": "official_bulk_employee_salary_no_names",
                "jurisdiction": "Montgomery County",
                "employer": "Montgomery County",
                "agency": row.get("department_name", row.get("department", "")),
                "person_name": "",
                "role_title": row.get("division", ""),
                "pay_grade": row.get("grade", ""),
                "annual_salary": norm_money(row.get("base_salary")),
                "gross_pay": "",
                "fiscal_year": "2024",
                "source_url": data_url,
                "provenance_note": "Official Montgomery County open data salary table. Source schema does not include employee names.",
            })
            count += 1
    catalog.append({
        "id": "montgomery_county_employee_salaries_2024",
        "name": meta.get("name", "Employee Salaries - 2024"),
        "jurisdiction": "Montgomery County",
        "kind": "official_bulk_employee_salary_no_names",
        "status": "downloaded",
        "records": count,
        "url": data_url,
        "description": meta.get("description", ""),
    })


def download_reference_docs(catalog):
    sources = [
        {
            "id": "maryland_dbm_salary_information",
            "name": "Maryland DBM Salary Information",
            "jurisdiction": "Maryland",
            "kind": "official_salary_schedule_reference",
            "url": "https://dbm.maryland.gov/employees/Pages/SalaryInformation.aspx",
        },
        {
            "id": "maryland_dbm_salary_plan",
            "name": "Maryland DBM Salary Plan",
            "jurisdiction": "Maryland",
            "kind": "official_classification_salary_plan",
            "url": "https://dbm.maryland.gov/employees/pages/salaryplan.aspx",
        },
        {
            "id": "umd_salary_structures",
            "name": "University of Maryland Salary Structures",
            "jurisdiction": "Maryland public university",
            "kind": "official_university_salary_structure_reference",
            "url": "https://uhr.umd.edu/employee-resources/classification-and-compensation/salary-structures",
        },
    ]
    DOCS.mkdir(parents=True, exist_ok=True)
    for src in sources:
        entry = dict(src)
        entry["status"] = "cataloged"
        try:
            html = fetch_text(src["url"])
            html_path = DOCS / f"{src['id']}.html"
            html_path.write_text(html, encoding="utf-8")
            entry["status"] = "downloaded_html"
            entry["local_html"] = str(html_path.relative_to(ROOT))
            pdfs = sorted(set(re.findall(r'href=["\']([^"\']+\\.pdf[^"\']*)["\']', html, flags=re.I)))
            downloaded = []
            for href in pdfs[:24]:
                full = urllib.parse.urljoin(src["url"], href)
                name = re.sub(r"[^A-Za-z0-9_.-]+", "_", urllib.parse.urlparse(full).path.split("/")[-1] or "document.pdf")
                out = DOCS / src["id"] / name
                out.parent.mkdir(parents=True, exist_ok=True)
                try:
                    out.write_bytes(fetch_bytes(full))
                    downloaded.append({"url": full, "local": str(out.relative_to(ROOT))})
                except Exception as e:
                    downloaded.append({"url": full, "error": str(e)})
            entry["downloaded_documents"] = downloaded
            entry["records"] = 0
            entry["note"] = "Official source provides salary schedules/structures, not a bulk named employee roster."
        except Exception as e:
            entry["status"] = "error"
            entry["error"] = str(e)
        catalog.append(entry)


def add_existing_hierarchy_people(rows, catalog):
    path = ROOT / "data" / "government" / "government_hierarchy_and_pay_2026.json"
    if not path.exists():
        return
    data = json.loads(path.read_text(encoding="utf-8"))
    count = 0
    for jurisdiction, label in [("maryland", "Maryland"), ("baltimore", "Baltimore City")]:
        node = data.get(jurisdiction, {})
        for section, items in node.get("sections", {}).items():
            if not isinstance(items, list):
                continue
            for item in items:
                name = item.get("name", "")
                person_like = (
                    "Governor " in name or
                    "Mayor " in name or
                    section == "Elected officials"
                )
                if not person_like:
                    continue
                rows.append({
                    "source_id": "government_hierarchy_and_pay_2026",
                    "source_type": "official_hierarchy_person_or_office",
                    "jurisdiction": label,
                    "employer": label,
                    "agency": section,
                    "person_name": name,
                    "role_title": section,
                    "pay_grade": "",
                    "annual_salary": "",
                    "gross_pay": "",
                    "fiscal_year": "2026",
                    "source_url": item.get("url", node.get("source", "")),
                    "provenance_note": "Existing hierarchy source; many entries are offices, not confirmed individual incumbents.",
                })
                count += 1
    catalog.append({
        "id": "government_hierarchy_and_pay_2026_people_like",
        "name": "Existing Government Hierarchy People-Like Entries",
        "jurisdiction": "Maryland / Baltimore / Federal",
        "kind": "derived_from_existing_hierarchy",
        "status": "normalized",
        "records": count,
        "url": str(path.relative_to(ROOT)),
    })


def add_federal_salient_positions(rows):
    path = ROOT / "data" / "government" / "government_hierarchy_and_pay_2026.json"
    if not path.exists():
        return
    data = json.loads(path.read_text(encoding="utf-8"))
    levels = data.get("federal", {}).get("pay_schedules", {}).get("executive_schedule", {}).get("levels", {})
    for row in data.get("federal", {}).get("salient_positions", []):
        schedule = row.get("pay_schedule", "")
        amount = ""
        for level, value in levels.items():
            if level in schedule:
                amount = norm_money(value)
                break
        rows.append({
            "source_id": "federal_salient_positions_2026",
            "source_type": "official_pay_reference_position",
            "jurisdiction": "Federal",
            "employer": "United States Government",
            "agency": row.get("branch", ""),
            "person_name": "",
            "role_title": row.get("position", ""),
            "pay_grade": schedule,
            "annual_salary": amount,
            "gross_pay": "",
            "fiscal_year": "2026",
            "source_url": row.get("pay_reference", ""),
            "provenance_note": "Position/pay reference. Incumbent name not present in source file.",
        })


def build_geographic_provenance(sources):
    buckets = {
        "Maryland": {
            "jurisdictions": {},
            "sources": []
        },
        "Federal": {
            "jurisdictions": {},
            "sources": []
        },
        "Other": {
            "jurisdictions": {},
            "sources": []
        },
    }

    def top_level(jurisdiction: str) -> str:
        j = (jurisdiction or "").strip().lower()
        if "federal" in j:
            return "Federal"
        if "maryland" in j or "baltimore" in j or "montgomery county" in j:
            return "Maryland"
        return "Other"

    for src in sources:
        jurisdiction = src.get("jurisdiction", "Unknown")
        key = top_level(jurisdiction)
        buckets[key]["sources"].append(src.get("id", ""))
        buckets[key]["jurisdictions"].setdefault(jurisdiction, []).append(src.get("id", ""))

    return {
        "model": "geographic",
        "regions": buckets,
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    catalog = []
    download_baltimore_city(rows, catalog)
    download_montgomery_county(rows, catalog)
    download_reference_docs(catalog)
    add_existing_hierarchy_people(rows, catalog)
    add_federal_salient_positions(rows)
    write_normalized(rows)
    geo_provenance = build_geographic_provenance(catalog)
    write_json(CATALOG, {
        "schema_version": 2,
        "generated_at_unix": int(time.time()),
        "privacy_boundary": "Public-sector employees/officials and official pay schedules only; no private Maryland residents.",
        "sources": catalog,
        "geographic_provenance": geo_provenance,
        "normalized": {
            "jsonl": str(NORM_JSONL.relative_to(ROOT)),
            "csv": str(NORM_CSV.relative_to(ROOT)),
            "records": len(rows),
        },
        "third_party_sources_not_auto_scraped": [
            {
                "name": "OpenGovPay Maryland",
                "url": "https://opengovpay.com/state/md",
                "reason": "Broad public salary aggregation exists, but licensing/provenance should be reviewed before bulk ingestion."
            },
            {
                "name": "GovSalaries Maryland",
                "url": "https://govsalaries.com/state/MD",
                "reason": "Third-party aggregator; not treated as official source for automatic ingestion."
            }
        ]
    })
    print(f"normalized_records={len(rows)}")
    print(f"catalog={CATALOG}")
    print(f"jsonl={NORM_JSONL}")
    print(f"csv={NORM_CSV}")


if __name__ == "__main__":
    main()
