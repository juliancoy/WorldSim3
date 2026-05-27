#pragma once

#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct BepsCandidate {
    size_t property_feature_idx = 0;
    size_t parcel_feature_idx = static_cast<size_t>(-1);
    std::string blocklot;
    std::string address;
    std::string owner;
    std::string use_group;
    std::string use_code;
    double gross_floor_area_sqft = 0.0;
    double beps_screened_area_sqft = 0.0;
    size_t grouped_record_count = 1;
    int dwelling_units = 0;
    bool area_threshold_met = false;
    bool commercial_like = false;
    bool multifamily_like = false;
    bool state_owned = false;
    bool federal_owner_exclusion_hint = false;
    bool possible_exemption_hint = false;
    bool might_be_covered = false;
    std::string confidence;
    std::string reason;
    std::string trigger;
};

struct BepsScreeningSummary {
    size_t property_record_count = 0;
    size_t candidate_count = 0;
    size_t matched_parcel_count = 0;
    size_t high_confidence_count = 0;
    size_t needs_review_count = 0;
};

std::unordered_map<std::string, size_t> buildParcelIndexByBlocklot(const LayerDef& parcel_layer);

std::vector<BepsCandidate> screenMarylandBepsCandidates(
    const LayerDef& property_layer,
    const LayerDef* parcel_layer,
    BepsScreeningSummary* summary = nullptr);

void applyBepsCandidateProperties(
    LayerDef& output_layer,
    const std::vector<BepsCandidate>& candidates);
