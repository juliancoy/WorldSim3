#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8787}"

echo "Clearing active query layers..."
curl -fsS -X POST "${BASE_URL}/controls/filter?action=clear_query_layers"
echo
echo

echo "Mode 1: show all Baltimore City parcels with a vacant notice in green..."
curl -fsS \
  "${BASE_URL}/controls/query?preset=vacant_notice_parcels&apply=layer&name=Vacant%20Notice%20Parcels&fill_color=%2300aa44cc&outline_color=%2300aa44ff&limit=50000"
echo
echo

echo "Clearing active query layers before categorical owner mode..."
curl -fsS -X POST "${BASE_URL}/controls/filter?action=clear_query_layers"
echo
echo

echo "Mode 2: split vacant-notice parcels into top 16 owners plus Other..."
curl -fsS \
  "${BASE_URL}/controls/query?preset=vacant_notice_top_owners&apply=layer&name=Vacant%20Notice%20Owners&top_n=16&limit=50000"
echo
