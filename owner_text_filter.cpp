#include "owner_text_filter.h"

#include "app_utils.h"

#include <algorithm>

namespace {
constexpr size_t kOwnerSearchIndexGramSize = 3;

bool indexInputsChanged(
    const OwnerTextFilterState& state,
    const OwnerTextFilterRefreshContext& ctx) {
    const std::string parcel_signature = ctx.parcel_signature ? *ctx.parcel_signature : std::string();
    const std::string real_property_signature = ctx.real_property_signature ? *ctx.real_property_signature : std::string();
    const size_t parcel_count = ctx.unified_parcels ? ctx.unified_parcels->size() : 0;
    return state.cached_parcel_signature != parcel_signature ||
        state.cached_real_property_signature != real_property_signature ||
        state.cached_parcel_count != parcel_count ||
        state.cached_real_property_count != ctx.real_property_count;
}

bool queryChanged(
    const OwnerTextFilterState& state,
    const std::string& normalized_query) {
    return state.cached_query != normalized_query;
}

uint32_t packTrigram(const std::string& text, size_t offset) {
    return (uint32_t)(uint8_t)text[offset] |
        ((uint32_t)(uint8_t)text[offset + 1] << 8) |
        ((uint32_t)(uint8_t)text[offset + 2] << 16);
}

void collectUniqueTrigrams(const std::string& text, std::vector<uint32_t>& out) {
    out.clear();
    if (text.size() < kOwnerSearchIndexGramSize) return;
    out.reserve(text.size() - (kOwnerSearchIndexGramSize - 1));
    for (size_t i = 0; i + kOwnerSearchIndexGramSize <= text.size(); ++i) {
        out.push_back(packTrigram(text, i));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void buildSearchIndex(
    OwnerTextSearchIndex& index,
    const std::vector<std::string>& owner_search_by_feature) {
    index.trigram_postings.clear();
    std::vector<uint32_t> grams;
    for (size_t feature_idx = 0; feature_idx < owner_search_by_feature.size(); ++feature_idx) {
        collectUniqueTrigrams(owner_search_by_feature[feature_idx], grams);
        for (uint32_t gram : grams) {
            index.trigram_postings[gram].push_back((uint32_t)feature_idx);
        }
    }
}

std::vector<uint32_t> candidateFeatureIndices(
    const OwnerTextSearchIndex& index,
    const std::string& normalized_query,
    const std::vector<std::string>& owner_search_by_feature) {
    if (normalized_query.size() < kOwnerSearchIndexGramSize) {
        std::vector<uint32_t> out;
        out.reserve(owner_search_by_feature.size());
        for (size_t i = 0; i < owner_search_by_feature.size(); ++i) {
            out.push_back((uint32_t)i);
        }
        return out;
    }

    std::vector<uint32_t> grams;
    collectUniqueTrigrams(normalized_query, grams);
    if (grams.empty()) return {};

    const std::vector<uint32_t>* smallest_posting = nullptr;
    std::vector<const std::vector<uint32_t>*> postings;
    postings.reserve(grams.size());
    for (uint32_t gram : grams) {
        auto it = index.trigram_postings.find(gram);
        if (it == index.trigram_postings.end()) return {};
        postings.push_back(&it->second);
        if (!smallest_posting || it->second.size() < smallest_posting->size()) {
            smallest_posting = &it->second;
        }
    }
    if (!smallest_posting) return {};

    std::vector<uint32_t> candidates = *smallest_posting;
    std::vector<uint32_t> scratch;
    for (const std::vector<uint32_t>* posting : postings) {
        if (posting == smallest_posting) continue;
        scratch.clear();
        scratch.reserve(std::min(candidates.size(), posting->size()));
        std::set_intersection(
            candidates.begin(), candidates.end(),
            posting->begin(), posting->end(),
            std::back_inserter(scratch));
        candidates.swap(scratch);
        if (candidates.empty()) break;
    }
    return candidates;
}
}

void refreshOwnerTextFilterState(
    OwnerTextFilterState& state,
    const OwnerTextFilterRefreshContext& ctx) {
    const std::string normalized_query =
        (ctx.map_filters && ctx.map_filters->enabled)
            ? toLowerAscii(trimDisplayValue(ctx.map_filters->owner))
            : std::string();

    const bool index_dirty = indexInputsChanged(state, ctx);
    if (index_dirty) {
        state.cached_parcel_signature = ctx.parcel_signature ? *ctx.parcel_signature : std::string();
        state.cached_real_property_signature = ctx.real_property_signature ? *ctx.real_property_signature : std::string();
        state.cached_parcel_count = ctx.unified_parcels ? ctx.unified_parcels->size() : 0;
        state.cached_real_property_count = ctx.real_property_count;
        state.search_index.trigram_postings.clear();
        if (ctx.parcel_owner_search_by_feature) {
            buildSearchIndex(state.search_index, *ctx.parcel_owner_search_by_feature);
        }
    }

    if (!index_dirty && !queryChanged(state, normalized_query)) {
        return;
    }

    state.cached_query = normalized_query;
    state.result_set = {};
    if (normalized_query.empty() ||
        !ctx.unified_parcels ||
        !ctx.parcel_owner_search_by_feature ||
        ctx.parcel_layer_idx < 0) {
        return;
    }

    state.result_set.active = true;
    state.result_set.layers.insert((size_t)ctx.parcel_layer_idx);
    if (ctx.real_property_layer_idx >= 0) {
        state.result_set.layers.insert((size_t)ctx.real_property_layer_idx);
    }

    const std::vector<uint32_t> candidates = candidateFeatureIndices(
        state.search_index,
        normalized_query,
        *ctx.parcel_owner_search_by_feature);
    for (uint32_t feature_idx : candidates) {
        if (feature_idx >= ctx.parcel_owner_search_by_feature->size()) continue;
        const std::string& owner_search = (*ctx.parcel_owner_search_by_feature)[feature_idx];
        if (owner_search.empty() || owner_search.find(normalized_query) == std::string::npos) continue;
        if (feature_idx >= ctx.unified_parcels->size()) continue;
        const UnifiedParcelRecord& row = (*ctx.unified_parcels)[feature_idx];
        state.result_set.features.insert(FeatureKey{row.parcel_layer_idx, row.parcel_local_feature_idx});
        if (!row.blocklot.empty()) {
            state.result_set.blocklots.insert(row.blocklot);
        }
    }
}
