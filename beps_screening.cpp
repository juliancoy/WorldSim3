#include "beps_screening.h"

#include "feature_props.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace {
constexpr double kMarylandBepsAreaThresholdSqFt = 35000.0;

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsLower(const std::string& haystack, const char* needle) {
    return lower(haystack).find(needle) != std::string::npos;
}

std::string compactKey(std::string value) {
    value = lower(value);
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(c));
    }
    return out;
}

double parseNumber(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), ','), value.end());
    value = trim(value);
    if (value.empty()) return 0.0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed)) return 0.0;
    return parsed;
}

int parseIntField(const LayerDef& layer, size_t feature_idx, std::initializer_list<const char*> keys) {
    return static_cast<int>(std::llround(parseNumber(getFirstPropertyValue(layer, feature_idx, keys))));
}

std::string joinedOwner(const LayerDef& layer, size_t feature_idx) {
    std::string owner = getFirstPropertyValue(layer, feature_idx, {"owner", "OWNER", "OWNER_1", "OWNERNAME", "TAXPAYER"});
    const std::string owner2 = getFirstPropertyValue(layer, feature_idx, {"OWNER_2"});
    const std::string owner3 = getFirstPropertyValue(layer, feature_idx, {"OWNER_3"});
    if (!trim(owner2).empty()) owner += " " + owner2;
    if (!trim(owner3).empty()) owner += " " + owner3;
    return trim(owner);
}

std::string candidateReason(const BepsCandidate& candidate) {
    std::vector<std::string> parts;
    parts.push_back(candidate.trigger + ">=" + std::to_string(static_cast<int>(kMarylandBepsAreaThresholdSqFt)));
    if (candidate.commercial_like) parts.push_back("commercial_like_use");
    if (candidate.multifamily_like) parts.push_back("multifamily_like_units");
    if (candidate.state_owned) parts.push_back("state_owned");
    if (candidate.federal_owner_exclusion_hint) parts.push_back("federal_owner_exclusion_hint");
    if (candidate.possible_exemption_hint) parts.push_back("possible_exemption_hint");

    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out << ";";
        out << parts[i];
    }
    return out.str();
}

void upsertProperty(LayerDef::FeatureProperties& props, const std::string& key, const std::string& value) {
    for (auto& kv : props.values) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    props.values.push_back({key, value});
}

struct BepsGroupStats {
    double area = 0.0;
    size_t count = 0;
    bool commercial_like = false;
    bool multifamily_like = false;
    bool state_owned = false;
};

std::string ownerKey(const LayerDef& layer, size_t feature_idx) {
    return compactKey(joinedOwner(layer, feature_idx));
}

std::string addressKey(const LayerDef& layer, size_t feature_idx) {
    return compactKey(getFirstPropertyValue(layer, feature_idx, {"FULLADDR", "address", "ADDRESS", "SITUSADDR", "PREMISE_ADDRESS"}));
}

std::string blockKey(const LayerDef& layer, size_t feature_idx) {
    return compactKey(getFirstPropertyValue(layer, feature_idx, {"BLOCK", "block", "Block"}));
}

bool commercialLike(const std::string& use_group, const std::string& use_code) {
    const std::string use_group_lower = lower(use_group);
    const std::string use_code_lower = lower(use_code);
    return use_group_lower.find('c') != std::string::npos ||
           containsLower(use_code_lower, "commercial") ||
           containsLower(use_code_lower, "office") ||
           containsLower(use_code_lower, "retail");
}

bool stateOwned(const std::string& owner) {
    const std::string owner_lower = lower(owner);
    return containsLower(owner_lower, "state of maryland") ||
           containsLower(owner_lower, "maryland state") ||
           containsLower(owner_lower, "md dept") ||
           containsLower(owner_lower, "maryland department");
}
}

std::unordered_map<std::string, size_t> buildParcelIndexByBlocklot(const LayerDef& parcel_layer) {
    std::unordered_map<std::string, size_t> out;
    for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
        const std::string blocklot = normalizeJoinKey(getFirstPropertyValue(
            parcel_layer,
            i,
            {"BLOCKLOT", "blocklot", "BlockLot", "PIN", "PINRELATE", "ACCOUNTID"}));
        if (!blocklot.empty() && !out.contains(blocklot)) out.emplace(blocklot, i);
    }
    return out;
}

std::vector<BepsCandidate> screenMarylandBepsCandidates(
    const LayerDef& property_layer,
    const LayerDef* parcel_layer,
    BepsScreeningSummary* summary) {
    std::vector<BepsCandidate> candidates;
    BepsScreeningSummary local_summary;
    local_summary.property_record_count = property_layer.features.size();

    std::unordered_map<std::string, size_t> parcel_by_blocklot;
    if (parcel_layer) parcel_by_blocklot = buildParcelIndexByBlocklot(*parcel_layer);

    std::unordered_map<std::string, BepsGroupStats> owner_address_groups;
    std::unordered_map<std::string, BepsGroupStats> owner_block_groups;
    for (size_t i = 0; i < property_layer.features.size(); ++i) {
        const double area = parseNumber(getFirstPropertyValue(
            property_layer,
            i,
            {"STRUCTAREA", "structure_area_sqft", "GROSS_AREA", "BLDG_AREA", "BUILDING_AREA", "LIVING_AREA"}));
        if (area <= 0.0) continue;
        const std::string owner = joinedOwner(property_layer, i);
        const std::string owner_key = ownerKey(property_layer, i);
        if (owner_key.empty()) continue;
        const std::string use_group = trim(getFirstPropertyValue(property_layer, i, {"USEGROUP", "use_group", "PROPERTY_USE"}));
        const std::string use_code = trim(getFirstPropertyValue(property_layer, i, {"DHCDUSE1", "SDATCODE", "USE_CODE", "LU"}));
        const int dwelling_units =
            parseIntField(property_layer, i, {"DWELUNIT", "dwelling_units", "UNITS"}) +
            parseIntField(property_layer, i, {"EFF_UNIT"}) +
            parseIntField(property_layer, i, {"ROOMUNIT"});
        auto update_group = [&](const std::string& key, std::unordered_map<std::string, BepsGroupStats>& groups) {
            if (key.empty()) return;
            BepsGroupStats& stats = groups[key];
            stats.area += area;
            stats.count += 1;
            stats.commercial_like = stats.commercial_like || commercialLike(use_group, use_code);
            stats.multifamily_like = stats.multifamily_like || dwelling_units >= 2;
            stats.state_owned = stats.state_owned || stateOwned(owner);
        };
        update_group(owner_key + "|" + addressKey(property_layer, i), owner_address_groups);
        update_group(owner_key + "|" + blockKey(property_layer, i), owner_block_groups);
    }

    for (size_t i = 0; i < property_layer.features.size(); ++i) {
        const double area = parseNumber(getFirstPropertyValue(
            property_layer,
            i,
            {"STRUCTAREA", "structure_area_sqft", "GROSS_AREA", "BLDG_AREA", "BUILDING_AREA", "LIVING_AREA"}));
        const std::string owner_key = ownerKey(property_layer, i);
        const std::string owner_address_key = owner_key + "|" + addressKey(property_layer, i);
        const std::string owner_block_key = owner_key + "|" + blockKey(property_layer, i);
        const auto owner_address_it = owner_address_groups.find(owner_address_key);
        const auto owner_block_it = owner_block_groups.find(owner_block_key);
        const double owner_address_area =
            owner_address_it == owner_address_groups.end() ? 0.0 : owner_address_it->second.area;
        const double owner_block_area =
            owner_block_it == owner_block_groups.end() ? 0.0 : owner_block_it->second.area;
        if (area < kMarylandBepsAreaThresholdSqFt &&
            owner_address_area < kMarylandBepsAreaThresholdSqFt &&
            owner_block_area < kMarylandBepsAreaThresholdSqFt) {
            continue;
        }

        BepsCandidate candidate;
        candidate.property_feature_idx = i;
        candidate.gross_floor_area_sqft = area;
        candidate.area_threshold_met = true;
        candidate.blocklot = normalizeJoinKey(getFirstPropertyValue(
            property_layer,
            i,
            {"BLOCKLOT", "blocklot", "BlockLot", "PIN", "PINRELATE", "ACCOUNTID"}));
        candidate.address = trim(getFirstPropertyValue(
            property_layer,
            i,
            {"FULLADDR", "address", "ADDRESS", "SITUSADDR", "PREMISE_ADDRESS"}));
        candidate.owner = joinedOwner(property_layer, i);
        candidate.use_group = trim(getFirstPropertyValue(property_layer, i, {"USEGROUP", "use_group", "PROPERTY_USE"}));
        candidate.use_code = trim(getFirstPropertyValue(property_layer, i, {"DHCDUSE1", "SDATCODE", "USE_CODE", "LU"}));
        candidate.dwelling_units =
            parseIntField(property_layer, i, {"DWELUNIT", "dwelling_units", "UNITS"}) +
            parseIntField(property_layer, i, {"EFF_UNIT"}) +
            parseIntField(property_layer, i, {"ROOMUNIT"});

        const std::string use_code_lower = lower(candidate.use_code);
        const std::string owner_lower = lower(candidate.owner);
        const std::string address_lower = lower(candidate.address);

        candidate.commercial_like = commercialLike(candidate.use_group, candidate.use_code);
        candidate.multifamily_like = candidate.dwelling_units >= 2;
        candidate.state_owned = stateOwned(candidate.owner);
        if (owner_address_it != owner_address_groups.end() && owner_address_it->second.area >= kMarylandBepsAreaThresholdSqFt) {
            candidate.commercial_like = candidate.commercial_like || owner_address_it->second.commercial_like;
            candidate.multifamily_like = candidate.multifamily_like || owner_address_it->second.multifamily_like;
            candidate.state_owned = candidate.state_owned || owner_address_it->second.state_owned;
        }
        if (owner_block_it != owner_block_groups.end() && owner_block_it->second.area >= kMarylandBepsAreaThresholdSqFt) {
            candidate.commercial_like = candidate.commercial_like || owner_block_it->second.commercial_like;
            candidate.multifamily_like = candidate.multifamily_like || owner_block_it->second.multifamily_like;
            candidate.state_owned = candidate.state_owned || owner_block_it->second.state_owned;
        }
        candidate.federal_owner_exclusion_hint =
            containsLower(owner_lower, "united states") ||
            containsLower(owner_lower, "u s govt") ||
            containsLower(owner_lower, "us govt") ||
            containsLower(owner_lower, "federal") ||
            containsLower(owner_lower, "gsa");
        candidate.possible_exemption_hint =
            containsLower(owner_lower, "school") ||
            containsLower(address_lower, "school") ||
            containsLower(use_code_lower, "manufact") ||
            containsLower(use_code_lower, "industrial") ||
            containsLower(lower(candidate.use_group), "m");

        if (area >= kMarylandBepsAreaThresholdSqFt) {
            candidate.beps_screened_area_sqft = area;
            candidate.grouped_record_count = 1;
            candidate.trigger = "individual_gross_floor_area_sqft";
        } else if (owner_address_area >= kMarylandBepsAreaThresholdSqFt) {
            candidate.beps_screened_area_sqft = owner_address_area;
            candidate.grouped_record_count = owner_address_it->second.count;
            candidate.trigger = "same_owner_same_address_structure_area_sqft";
        } else {
            candidate.beps_screened_area_sqft = owner_block_area;
            candidate.grouped_record_count = owner_block_it->second.count;
            candidate.trigger = "same_owner_same_block_structure_area_sqft";
        }
        candidate.might_be_covered =
            candidate.beps_screened_area_sqft >= kMarylandBepsAreaThresholdSqFt &&
            !candidate.federal_owner_exclusion_hint &&
            (candidate.commercial_like || candidate.multifamily_like || candidate.state_owned);
        if (!candidate.might_be_covered) continue;

        candidate.confidence = candidate.possible_exemption_hint || candidate.grouped_record_count > 1 ? "needs_review" : "high";
        candidate.reason = candidateReason(candidate);
        if (parcel_layer && !candidate.blocklot.empty()) {
            auto it = parcel_by_blocklot.find(candidate.blocklot);
            if (it != parcel_by_blocklot.end()) {
                candidate.parcel_feature_idx = it->second;
                local_summary.matched_parcel_count += 1;
            }
        }
        if (candidate.confidence == "high") local_summary.high_confidence_count += 1;
        else local_summary.needs_review_count += 1;
        candidates.push_back(std::move(candidate));
    }

    local_summary.candidate_count = candidates.size();
    if (summary) *summary = local_summary;
    return candidates;
}

void applyBepsCandidateProperties(
    LayerDef& output_layer,
    const std::vector<BepsCandidate>& candidates) {
    output_layer.feature_properties.resize(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const BepsCandidate& candidate = candidates[i];
        LayerDef::FeatureProperties& props = output_layer.feature_properties[i];
        upsertProperty(props, "beps_screening_algorithm", "comar_26_28_01_parcel_candidate_screen");
        upsertProperty(props, "beps_might_be_covered", candidate.might_be_covered ? "true" : "false");
        upsertProperty(props, "beps_confidence", candidate.confidence);
        upsertProperty(props, "beps_reason", candidate.reason);
        upsertProperty(props, "gross_floor_area_sqft", std::to_string(candidate.gross_floor_area_sqft));
        upsertProperty(props, "beps_screened_area_sqft", std::to_string(candidate.beps_screened_area_sqft));
        upsertProperty(props, "beps_grouped_record_count", std::to_string(candidate.grouped_record_count));
        upsertProperty(props, "beps_trigger", candidate.trigger);
        upsertProperty(props, "dwelling_units", std::to_string(candidate.dwelling_units));
        upsertProperty(props, "blocklot", candidate.blocklot);
        upsertProperty(props, "address", candidate.address);
        upsertProperty(props, "owner", candidate.owner);
        upsertProperty(props, "use_group", candidate.use_group);
        upsertProperty(props, "use_code", candidate.use_code);
        upsertProperty(props, "possible_exemption_hint", candidate.possible_exemption_hint ? "true" : "false");
        upsertProperty(props, "federal_owner_exclusion_hint", candidate.federal_owner_exclusion_hint ? "true" : "false");
    }
}
