#!/usr/bin/env python3
import csv
import json
import pathlib
import subprocess
import collections

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA = ROOT / 'data' / 'layers'
YEAR = 2024
COUNTY_FIPS = '24510'


def fetch_csv(url: str) -> str:
    cp = subprocess.run(['curl', '-L', '-A', 'Mozilla/5.0', url], check=True, capture_output=True, text=True)
    return cp.stdout


def main() -> int:
    DATA.mkdir(parents=True, exist_ok=True)
    tract_geo = DATA / 'cdc_places_total_population_baltimore_tracts.geojson'
    if not tract_geo.exists():
        tract_geo = DATA / 'cdc_places_asthma_baltimore_tracts.geojson'
    if not tract_geo.exists():
        raise SystemExit('missing tract geometry source; download a CDC PLACES tract layer first')

    base = f'https://ffiec.cfpb.gov/v2/data-browser-api/view/csv?counties={COUNTY_FIPS}&years={YEAR}'
    all_csv = fetch_csv(base)
    org_csv = fetch_csv(base + '&actions_taken=1')
    den_csv = fetch_csv(base + '&actions_taken=3')

    agg = collections.defaultdict(lambda: {
        'applications': 0,
        'originations': 0,
        'denials': 0,
        'loan_sum': 0.0,
        'property_sum': 0.0,
    })

    def parse_rows(text: str, mode: str) -> None:
        rd = csv.DictReader(text.splitlines())
        for r in rd:
            tract = (r.get('census_tract') or '').strip()
            if not tract or tract == 'NA':
                continue
            a = agg[tract]
            if mode == 'all':
                a['applications'] += 1
            elif mode == 'org':
                a['originations'] += 1
                try:
                    a['loan_sum'] += float(r.get('loan_amount') or 0)
                except Exception:
                    pass
                try:
                    a['property_sum'] += float(r.get('property_value') or 0)
                except Exception:
                    pass
            elif mode == 'den':
                a['denials'] += 1

    parse_rows(all_csv, 'all')
    parse_rows(org_csv, 'org')
    parse_rows(den_csv, 'den')

    geom = json.loads(tract_geo.read_text())
    out = {'type': 'FeatureCollection', 'features': []}
    for ft in geom.get('features', []):
        p = dict(ft.get('properties', {}))
        tract = str(p.get('TractFIPS') or p.get('FIPS') or '').strip()
        if not tract:
            continue
        a = agg.get(tract, {})
        apps = a.get('applications', 0)
        org = a.get('originations', 0)
        den = a.get('denials', 0)
        loan = a.get('loan_sum', 0.0)
        prop = a.get('property_sum', 0.0)
        p['HMDA_Year'] = YEAR
        p['HMDA_Applications'] = apps
        p['HMDA_Originations'] = org
        p['HMDA_Denials'] = den
        p['HMDA_DenialRate'] = (den / apps) if apps > 0 else 0.0
        p['HMDA_OrigLoanSum'] = loan
        p['HMDA_AvgLoanAmount'] = (loan / org) if org > 0 else 0.0
        p['HMDA_OrigPropertySum'] = prop
        out['features'].append({'type': 'Feature', 'geometry': ft.get('geometry'), 'properties': p})

    out_path = DATA / 'hmda_mortgage_2024_baltimore_tracts.geojson'
    out_path.write_text(json.dumps(out))
    print(f'wrote {out_path} ({len(out["features"])} tracts)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
