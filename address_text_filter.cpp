#include "address_text_filter.h"

#include "app_utils.h"

#include <algorithm>

namespace {
constexpr size_t kAddressSearchIndexGramSize = 3;

bool indexInputsChanged(
    const AddressTextFilterState& state,
    const AddressTextFilterRefreshContext& ctx) {
    const std::string parcel_signature = ctx.parcel_signature ? *ctx.parcel_signature : std::string();
    const size_t parcel_count = ctx.unified_parcels ? ctx.unified_parcels->size() : 0;
    return state.cached_parcel_signature != parcel_signature ||
        state.cached_parcel_count != parcel_count;
}

bool queryChanged(
    const AddressTextFilterState& state,
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
    if (text.size() < kAddressSearchIndexGramSize) return;
    out.reserve(text.size() - (kAddressSearchIndexGramSize - 1));
    for (size_t i = 0; i + kAddressSearchIndexGramSize <= text.size(); ++i) {
        out.push_back(packTrigram(text, i));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void buildSearchIndex(
    AddressTextSearchIndex& index,
    const std::vector<std::string>& address_search_by_feature) {
    index.trigram_postings.clear();
    std::vector<uint32_t> grams;
    for (size_t feature_idx = 0; feature_idx < address_search_by_feature.size(); ++feature_idx) {
        collectUniqueTrigrams(address_search_by_feature[feature_idx], grams);
        for (uint32_t gram : grams) {
            index.trigram_postings[gram].push_back((uint32_t)feature_idx);
        }
    }
}

std::vector<uint32_t> candidateFeatureIndices(
    const AddressTextSearchIndex& index,
    const std::string& normalized_query,
    const std::vector<std::string>& address_search_by_feature) {
    if (normalized_query.size() < kAddressSearchIndexGramSize) {
        std::vector<uint32_t> out;
        out.reserve(address_search_by_feature.size());
        for (size_t i = 0; i < address_search_by_feature.size(); ++i) {
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

void refreshAddressTextFilterState(
    AddressTextFilterState& state,
    const AddressTextFilterRefreshContext& ctx) {
    const std::string normalized_query =
        (ctx.map_filters && ctx.map_filters->enabled)
            ? normalizeAddressSearchText(trimDisplayValue(ctx.map_filters->address))
            : std::string();

    const bool index_dirty = indexInputsChanged(state, ctx);
    if (index_dirty) {
        state.cached_parcel_signature = ctx.parcel_signature ? *ctx.parcel_signature : std::string();
        state.cached_parcel_count = ctx.unified_parcels ? ctx.unified_parcels->size() : 0;
        state.search_index.trigram_postings.clear();
        if (ctx.parcel_address_search_by_feature) {
            buildSearchIndex(state.search_index, *ctx.parcel_address_search_by_feature);
        }
    }

    if (!index_dirty && !queryChanged(state, normalized_query)) {
        return;
    }

    state.cached_query = normalized_query;
    state.result_set = {};
    if (normalized_query.empty() ||
        !ctx.unified_parcels ||
        !ctx.parcel_address_search_by_feature ||
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
        *ctx.parcel_address_search_by_feature);
    for (uint32_t feature_idx : candidates) {
        if (feature_idx >= ctx.parcel_address_search_by_feature->size()) continue;
        const std::string& address_search = (*ctx.parcel_address_search_by_feature)[feature_idx];
        if (address_search.empty() || address_search.find(normalized_query) == std::string::npos) continue;
        const UnifiedParcelRecord* row = unifiedParcelAt(*ctx.unified_parcels, feature_idx);
        if (!row) continue;
        state.result_set.features.insert(FeatureKey{row->parcel_layer_idx, row->parcel_feature_idx});
        if (!row->blocklot.empty()) {
            state.result_set.blocklots.insert(row->blocklot);
        }
    }
}
