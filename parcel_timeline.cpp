#include "parcel_timeline.h"

#include "app_utils.h"
#include "feature_props.h"
#include "layer_geometry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <unordered_map>
#include <string>

namespace {

std::string firstNonemptyProp(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = trimDisplayValue(getPropertyValue(fg, k));
        if (!v.empty()) return v;
    }
    return {};
}

int findLayerIdxByFile(const std::vector<LayerDef>& layers, const char* file_name) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == file_name) return (int)i;
    }
    return -1;
}

bool extentsOverlap(const LayerDef::FeatureGeom& a, const LayerDef::FeatureGeom& b) {
    return !(a.extent.max_lon < b.extent.min_lon ||
             a.extent.min_lon > b.extent.max_lon ||
             a.extent.max_lat < b.extent.min_lat ||
             a.extent.min_lat > b.extent.max_lat);
}

std::string digitsOnly(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isdigit(ch)) out.push_back((char)ch);
    }
    return out;
}

bool parseIsoDateParts(
    const std::string& s,
    int& year,
    int& month,
    int& day,
    bool& has_time,
    int& hour,
    int& minute) {
    if (s.size() < 10) return false;
    if (!(std::isdigit((unsigned char)s[0]) &&
          std::isdigit((unsigned char)s[1]) &&
          std::isdigit((unsigned char)s[2]) &&
          std::isdigit((unsigned char)s[3]) &&
          s[4] == '-' &&
          std::isdigit((unsigned char)s[5]) &&
          std::isdigit((unsigned char)s[6]) &&
          s[7] == '-' &&
          std::isdigit((unsigned char)s[8]) &&
          std::isdigit((unsigned char)s[9]))) {
        return false;
    }
    year = std::stoi(s.substr(0, 4));
    month = std::stoi(s.substr(5, 2));
    day = std::stoi(s.substr(8, 2));
    has_time = false;
    hour = 0;
    minute = 0;
    if (s.size() >= 16 && (s[10] == 'T' || s[10] == ' ')) {
        if (std::isdigit((unsigned char)s[11]) &&
            std::isdigit((unsigned char)s[12]) &&
            s[13] == ':' &&
            std::isdigit((unsigned char)s[14]) &&
            std::isdigit((unsigned char)s[15])) {
            has_time = true;
            hour = std::stoi(s.substr(11, 2));
            minute = std::stoi(s.substr(14, 2));
        }
    }
    return true;
}

std::string normalizedTimelineDateDisplay(const std::string& raw) {
    const std::string d = trimDisplayValue(raw);
    if (d.empty()) return {};

    int year = 0, month = 0, day = 0;
    bool has_time = false;
    int hour = 0, minute = 0;
    if (parseIsoDateParts(d, year, month, day, has_time, hour, minute)) {
        char buf[64];
        if (has_time) {
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d UTC", year, month, day, hour, minute);
        } else {
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
        }
        return std::string(buf);
    }

    const std::string nums = digitsOnly(d);
    if (nums.size() == 8) {
        const int tail_year = std::stoi(nums.substr(4, 4));
        if (tail_year >= 1900 && tail_year <= 2100) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tail_year, std::stoi(nums.substr(0, 2)), std::stoi(nums.substr(2, 2)));
            return std::string(buf);
        }
        const int head_year = std::stoi(nums.substr(0, 4));
        if (head_year >= 1900 && head_year <= 2100) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", head_year, std::stoi(nums.substr(4, 2)), std::stoi(nums.substr(6, 2)));
            return std::string(buf);
        }
    }

    const int maybe_year = extractYearMaybe(d);
    if (maybe_year >= 1900 && maybe_year <= 2100) return std::to_string(maybe_year);
    return d;
}

std::string clarifyTimelineStatus(const std::string& raw_status) {
    const std::string s = trimDisplayValue(raw_status);
    if (s.empty()) return {};
    static const std::unordered_map<std::string, std::string> kSdatUseGroupLabels = {
        {"R", "Residential"},
        {"C", "Commercial"},
        {"I", "Industrial"},
        {"M", "Apartments (rental multi-unit)"},
        {"E", "Exempt"},
        {"U", "Utility"},
    };
    if (s.size() == 1 && std::isalnum((unsigned char)s[0])) {
        std::string code(1, (char)std::toupper((unsigned char)s[0]));
        auto it = kSdatUseGroupLabels.find(code);
        if (it != kSdatUseGroupLabels.end()) {
            return code + " (" + it->second + ")";
        }
        return code + " (source status code: " + code + ")";
    }
    return s;
}

std::string sortKeyForDate(const std::string& date) {
    const std::string d = trimDisplayValue(date);
    if (d.size() >= 10 &&
        std::isdigit((unsigned char)d[0]) &&
        std::isdigit((unsigned char)d[1]) &&
        std::isdigit((unsigned char)d[2]) &&
        std::isdigit((unsigned char)d[3]) &&
        d[4] == '-') {
        return d.substr(0, 10);
    }

    const std::string nums = digitsOnly(d);
    if (nums.size() == 8) {
        const int tail_year = std::stoi(nums.substr(4, 4));
        if (tail_year >= 1900 && tail_year <= 2100) {
            return nums.substr(4, 4) + "-" + nums.substr(0, 2) + "-" + nums.substr(2, 2);
        }
        const int head_year = std::stoi(nums.substr(0, 4));
        if (head_year >= 1900 && head_year <= 2100) {
            return nums.substr(0, 4) + "-" + nums.substr(4, 2) + "-" + nums.substr(6, 2);
        }
    }

    const int year = extractYearMaybe(d);
    if (year >= 1900 && year <= 2100) return std::to_string(year) + "-00-00";
    return "0000-00-00";
}

void addEvent(
    std::vector<ParcelTimelineEvent>& out,
    std::string date,
    std::string event_type,
    std::string status,
    std::string amount,
    std::string source_layer) {
    ParcelTimelineEvent row;
    row.date = normalizedTimelineDateDisplay(date);
    row.event_type = trimDisplayValue(std::move(event_type));
    row.status = clarifyTimelineStatus(status);
    row.amount = trimDisplayValue(std::move(amount));
    row.source_layer = trimDisplayValue(std::move(source_layer));
    row.sort_key = sortKeyForDate(row.date);
    out.push_back(std::move(row));
}

void appendOwnershipTransfer(
    const LayerDef::FeatureGeom* rp,
    std::vector<ParcelTimelineEvent>& out) {
    if (!rp) return;
    const std::string sale_date = firstNonemptyProp(*rp, {"SALEDATE", "SALE_DATE", "TRANSFER_DATE", "DEED_DATE"});
    const std::string owner = firstNonemptyProp(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME"});
    const std::string deed_book = firstNonemptyProp(*rp, {"DEEDBOOK", "DEED_BOOK"});
    const std::string deed_page = firstNonemptyProp(*rp, {"DEEDPAGE", "DEED_PAGE"});
    const std::string sale_price = firstNonemptyProp(*rp, {"SALEPRIC", "SALE_PRICE", "PRICE", "CONSIDERATION"});
    if (sale_date.empty() && owner.empty() && deed_book.empty() && deed_page.empty() && sale_price.empty()) return;

    std::string status;
    if (!owner.empty()) status = "Owner: " + owner;
    if (!deed_book.empty() || !deed_page.empty()) {
        if (!status.empty()) status += " | ";
        status += "Deed";
        if (!deed_book.empty()) status += " " + deed_book;
        if (!deed_page.empty()) status += "/" + deed_page;
    }
    addEvent(out, sale_date, "Ownership Transfer / Sale", status, sale_price, "Real Property Information");
}

void appendAssessmentSnapshot(
    const LayerDef::FeatureGeom* rp,
    std::vector<ParcelTimelineEvent>& out) {
    if (!rp) return;
    const std::string ldate = firstNonemptyProp(*rp, {"LDATE", "LAST_UPDATE", "UPDATED_DATE"});
    if (ldate.empty()) return;
    const std::string tax_base = firstNonemptyProp(*rp, {"TAXBASE", "ARTAXBAS"});
    const std::string status = firstNonemptyProp(*rp, {"USEGROUP", "DHCDUSE1", "ZONECODE"});
    addEvent(out, ldate, "Assessment / Property Record Update", status, tax_base, "Real Property Information");
}

void appendEventsFromBlocklotLayer(
    const std::vector<LayerDef>& layers,
    int layer_idx,
    const std::string& parcel_blocklot,
    const char* event_type,
    std::initializer_list<const char*> date_keys,
    std::initializer_list<const char*> status_keys,
    std::initializer_list<const char*> amount_keys,
    std::vector<ParcelTimelineEvent>& out) {
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size() || parcel_blocklot.empty()) return;
    const auto& layer = layers[(size_t)layer_idx];
    for (const auto& feat : layer.features) {
        std::string ev_blocklot = featureBlockLotJoinKey(feat);
        if (ev_blocklot.empty()) ev_blocklot = normalizeJoinKey(firstNonemptyProp(feat, {"BlockLot", "blocklot"}));
        if (ev_blocklot.empty() || ev_blocklot != parcel_blocklot) continue;
        addEvent(
            out,
            firstNonemptyProp(feat, date_keys),
            event_type,
            firstNonemptyProp(feat, status_keys),
            firstNonemptyProp(feat, amount_keys),
            layer.name);
    }
}

void appendEventsFromSpatialLayer(
    const std::vector<LayerDef>& layers,
    int layer_idx,
    const LayerDef::FeatureGeom& parcel,
    const char* event_type,
    std::initializer_list<const char*> date_keys,
    std::initializer_list<const char*> status_keys,
    std::initializer_list<const char*> amount_keys,
    std::vector<ParcelTimelineEvent>& out) {
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) return;
    const auto& layer = layers[(size_t)layer_idx];
    const float parcel_cx = (parcel.extent.min_lon + parcel.extent.max_lon) * 0.5f;
    const float parcel_cy = (parcel.extent.min_lat + parcel.extent.max_lat) * 0.5f;
    for (const auto& feat : layer.features) {
        bool related = false;
        if (feat.rings.empty()) {
            related = pointInFeature(parcel, feat.extent.min_lon, feat.extent.min_lat);
        } else {
            related = pointInFeature(feat, parcel_cx, parcel_cy) || extentsOverlap(parcel, feat);
        }
        if (!related) continue;
        addEvent(
            out,
            firstNonemptyProp(feat, date_keys),
            event_type,
            firstNonemptyProp(feat, status_keys),
            firstNonemptyProp(feat, amount_keys),
            layer.name);
    }
}

void appendParcelIntelligenceEvent(
    const std::vector<LayerDef>& layers,
    const std::string& parcel_blocklot,
    std::vector<ParcelTimelineEvent>& out) {
    const int layer_idx = findLayerIdxByFile(layers, "parcel_intelligence.geojson");
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size() || parcel_blocklot.empty()) return;
    const auto& layer = layers[(size_t)layer_idx];
    for (const auto& feat : layer.features) {
        if (featureBlockLotJoinKey(feat) != parcel_blocklot) continue;
        const std::string latest = firstNonemptyProp(feat, {"latest_event_date"});
        if (latest.empty()) continue;
        std::string status = firstNonemptyProp(feat, {"risk_band", "risk_drivers"});
        const std::string drivers = firstNonemptyProp(feat, {"risk_drivers"});
        if (!drivers.empty() && status.find(drivers) == std::string::npos) {
            if (!status.empty()) status += " | ";
            status += drivers;
        }
        addEvent(
            out,
            latest,
            "Parcel Intelligence Signal",
            status,
            firstNonemptyProp(feat, {"risk_score", "investment_5y_usd", "tax_lien_amount_usd"}),
            layer.name);
        return;
    }
}

} // namespace

std::vector<ParcelTimelineEvent> buildParcelTimeline(const ParcelTimelineRequest& request) {
    std::vector<ParcelTimelineEvent> out;
    if (!request.layers || !request.parcel) return out;

    const auto& layers = *request.layers;
    const auto& parcel = *request.parcel;
    const std::string blocklot = featureBlockLotJoinKey(parcel);

    appendOwnershipTransfer(request.real_property, out);
    appendAssessmentSnapshot(request.real_property, out);

    appendEventsFromBlocklotLayer(layers, request.vacant_notice_layer_idx, blocklot, "Vacant Notice",
        {"DateNotice", "DateAbate", "DateCancel", "DATE", "RECORD_DATE", "DateIssue", "DateIssued", "CREATED_DATE"},
        {"Status", "STATUS", "CASE_STATUS", "STATE"},
        {"Amount", "AMOUNT", "LIENAMOUNT", "TOTAL_LIEN"}, out);
    appendEventsFromBlocklotLayer(layers, request.vacant_rehab_layer_idx, blocklot, "Vacant Rehab",
        {"DateIssue", "DateIssued", "DATE", "RECORD_DATE", "CREATED_DATE"},
        {"Status", "STATUS", "CASE_STATUS", "STATE", "Description"},
        {"Amount", "AMOUNT", "LIENAMOUNT", "TOTAL_LIEN"}, out);
    appendEventsFromBlocklotLayer(layers, request.tax_lien_layer_idx, blocklot, "Tax Lien",
        {"REDEMPTION_DATE", "DATE", "RECORD_DATE", "DateIssue", "DateIssued", "CREATED_DATE"},
        {"Status", "STATUS", "STATE"},
        {"TOTAL_AMOUNT", "LIENS", "LIENAMOUNT", "TOTAL_LIEN", "Amount", "AMOUNT"}, out);
    appendEventsFromBlocklotLayer(layers, request.tax_sale_layer_idx, blocklot, "Tax Sale",
        {"deed_date", "when_sold", "DATE", "RECORD_DATE", "DateIssue", "DateIssued", "CREATED_DATE"},
        {"Status", "STATUS", "STATE"},
        {"total_lien", "total_li_1", "total_3yea", "TOTAL_LIEN", "LIENAMOUNT", "Amount", "AMOUNT"}, out);

    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "housing_building_permits_2019_present.geojson"), blocklot, "Building Permit",
        {"IssuedDate", "DATE", "RECORD_DATE", "CREATED_DATE"},
        {"Description", "STATUS", "STATE", "ExistingUse", "ProposedUse"},
        {"Cost", "AMOUNT", "Amount", "TOTAL"}, out);
    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "housing_building_permits_2015_2018.geojson"), blocklot, "Building Permit",
        {"IssuedDate", "DATE", "RECORD_DATE", "CREATED_DATE"},
        {"Description", "STATUS", "STATE", "ExistingUse", "ProposedUse"},
        {"Cost", "AMOUNT", "Amount", "TOTAL"}, out);

    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "open_bid_list_vacants_to_value.geojson"), blocklot, "Vacants to Value Bid Opportunity",
        {"DATE", "RECORD_DATE", "CREATED_DATE", "DateUpdate"},
        {"Status", "STATUS", "Neighborhood", "HousingMarketTypology2017"},
        {"Amount", "AMOUNT", "TOTAL"}, out);
    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "completed_city_demo.geojson"), blocklot, "City Demolition Complete",
        {"DateDemoFinished", "DateStarted", "ReleasedToContractor", "DateUpdate"},
        {"SimplifiedStatus", "GroupStatus", "CorePhase"},
        {"Amount", "AMOUNT", "TOTAL"}, out);

    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "foreclosure_filings.geojson"), blocklot, "Foreclosure Filing",
        {"Date", "DATE", "RECORD_DATE", "DateFiled", "FILED_DATE", "CREATED_DATE"},
        {"Status", "STATUS", "STATE", "CASE_STATUS"},
        {"Amount", "AMOUNT", "TOTAL", "TOTAL_LIEN"}, out);
    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "receivership_filed_open.geojson"), blocklot, "Receivership (Open)",
        {"DateFiled", "DATE", "RECORD_DATE", "FILED_DATE", "CREATED_DATE"},
        {"Status", "STATUS", "STATE", "CASE_STATUS"},
        {"Amount", "AMOUNT", "TOTAL", "TOTAL_LIEN"}, out);
    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "receivership_settled.geojson"), blocklot, "Receivership (Settled)",
        {"DateFiled", "DateAuction", "DATE", "RECORD_DATE", "FILED_DATE", "CREATED_DATE"},
        {"Status", "STATUS", "STATE", "CASE_STATUS"},
        {"Amount", "AMOUNT", "TOTAL", "TOTAL_LIEN"}, out);
    appendEventsFromBlocklotLayer(layers, findLayerIdxByFile(layers, "open_work_orders.geojson"), blocklot, "Open Work Order",
        {"DATE", "RECORD_DATE", "DateIssued", "DateNotice", "CREATED_DATE"},
        {"Status", "STATUS", "STATE", "CASE_STATUS", "Description"},
        {"Amount", "AMOUNT", "TOTAL", "TOTAL_LIEN"}, out);

    appendEventsFromSpatialLayer(layers, findLayerIdxByFile(layers, "cip_fy14_20_projects_point.geojson"), parcel, "CIP Project Allocation",
        {"FY", "FiscalYear", "DATE"},
        {"Project_Title", "Name", "agency", "Project_Name"},
        {"Totals_1", "City_Bond_Funds", "Federal_Funds", "State_Funds", "Utility_Funds", "Revenue_Loans"}, out);
    appendEventsFromSpatialLayer(layers, findLayerIdxByFile(layers, "dpw_cip_storm_ms4_projects.geojson"), parcel, "DPW CIP Storm MS4 Project",
        {"FY", "FiscalYear", "DATE", "CREATED_DATE"},
        {"Project_Title", "ProjectName", "Name", "Status"},
        {"Amount", "AMOUNT", "TOTAL", "Total"}, out);
    appendEventsFromSpatialLayer(layers, findLayerIdxByFile(layers, "dpw_cip_stormwater_projects.geojson"), parcel, "DPW CIP Stormwater Project",
        {"FY", "FiscalYear", "DATE", "CREATED_DATE"},
        {"Project_Title", "ProjectName", "Name", "Status"},
        {"Amount", "AMOUNT", "TOTAL", "Total"}, out);
    appendEventsFromSpatialLayer(layers, findLayerIdxByFile(layers, "dpw_cip_water_projects.geojson"), parcel, "DPW CIP Water Project",
        {"FY", "FiscalYear", "DATE", "CREATED_DATE"},
        {"Project_Title", "ProjectName", "Name", "Status"},
        {"Amount", "AMOUNT", "TOTAL", "Total"}, out);
    appendEventsFromSpatialLayer(layers, findLayerIdxByFile(layers, "dpw_cip_wastewater_projects.geojson"), parcel, "DPW CIP Wastewater Project",
        {"FY", "FiscalYear", "DATE", "CREATED_DATE"},
        {"Project_Title", "ProjectName", "Name", "Status"},
        {"Amount", "AMOUNT", "TOTAL", "Total"}, out);

    appendParcelIntelligenceEvent(layers, blocklot, out);

    std::sort(out.begin(), out.end(), [](const ParcelTimelineEvent& a, const ParcelTimelineEvent& b) {
        if (a.sort_key != b.sort_key) return a.sort_key > b.sort_key;
        if (a.event_type != b.event_type) return a.event_type < b.event_type;
        return a.source_layer < b.source_layer;
    });
    return out;
}
