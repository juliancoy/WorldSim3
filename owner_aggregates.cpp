#include "owner_aggregates.h"

#include "app_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <numbers>

using json = nlohmann::json;

namespace {
double profMsSince(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

bool containsAny(const std::string& s, std::initializer_list<const char*> terms) {
    for (const char* t : terms) {
        if (s.find(t) != std::string::npos) return true;
    }
    return false;
}

std::string classifyOwner(const std::string& owner) {
    if (owner.empty()) return "unknown";
    const std::string& s = owner;
    if (containsAny(s, {"receiver", "receivership", "trustee", "court appointed", "special receiver"})) return "receivership";
    if (containsAny(s, {"city of ", "county", "state of ", "housing authority", "dept of", "department of", "u.s.", " us ", "usa"})) return "government";
    if (containsAny(s, {"development corp", "community development", "cdc ", " cdc", "redevelopment"})) return "development_corp";
    if (containsAny(s, {"church", "ministries", "foundation", "nonprofit", "charitable", "association"})) return "nonprofit";
    if (containsAny(s, {"trust", "estate"})) return "trust_or_estate";
    if (containsAny(s, {"bank", "credit union", "mortgage", "federal national", "fannie mae", "freddie mac", "loan"})) return "financial_institution";
    if (containsAny(s, {" llc", "llc ", "inc", "corp", "company", " co ", " ltd", "lp", "llp", "properties", "holdings"})) return "llc_corporate";
    int alpha_tokens = 0;
    bool has_non_alpha_token = false;
    std::string token;
    for (char ch : s) {
        if (std::isalpha((unsigned char)ch)) token.push_back(ch);
        else if (!token.empty()) {
            alpha_tokens++;
            token.clear();
        } else if (!std::isspace((unsigned char)ch)) {
            has_non_alpha_token = true;
        }
    }
    if (!token.empty()) alpha_tokens++;
    if (!has_non_alpha_token && alpha_tokens >= 2 && alpha_tokens <= 4) return "individual";
    return "unknown";
}

double parcelAreaSqM(const LayerDef::FeatureGeom& fg) {
    if (fg.rings.empty()) return 0.0;
    const double deg_to_m_lat = 111320.0;
    double total = 0.0;
    for (const auto& ring : fg.rings) {
        if (ring.size() < 3) continue;
        double lat_sum = 0.0;
        for (const auto& p : ring) lat_sum += (double)p.y;
        const double lat0 = lat_sum / (double)ring.size();
        const double cos_lat = std::cos(lat0 * std::numbers::pi / 180.0);
        const double sx = deg_to_m_lat * cos_lat;
        const double sy = deg_to_m_lat;
        double a = 0.0;
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % n];
            const double px = (double)p.x * sx;
            const double py = (double)p.y * sy;
            const double qx = (double)q.x * sx;
            const double qy = (double)q.y * sy;
            a += (px * qy - qx * py);
        }
        total += std::abs(a) * 0.5;
    }
    return total;
}

void loadOwnerClassOverrides(const OwnerAggregatesContext& ctx) {
    if (!ctx.owner_class_overrides_loaded || !ctx.owner_class_overrides) return;
    if (*ctx.owner_class_overrides_loaded) return;
    *ctx.owner_class_overrides_loaded = true;
    ctx.owner_class_overrides->clear();
    std::ifstream in(*ctx.root / "data" / "owner_class_overrides.json");
    if (!in) return;
    try {
        json oj;
        in >> oj;
        if (!oj.is_object()) return;
        for (auto it = oj.begin(); it != oj.end(); ++it) {
            if (it.value().is_string()) {
                (*ctx.owner_class_overrides)[it.key()] = it.value().get<std::string>();
            }
        }
    } catch (...) {
    }
}

void persistOwnerClassOverrides(const OwnerAggregatesContext& ctx) {
    if (!ctx.owner_class_overrides_dirty || !ctx.owner_class_overrides) return;
    if (!*ctx.owner_class_overrides_dirty) return;
    *ctx.owner_class_overrides_dirty = false;
    std::filesystem::create_directories(*ctx.root / "data");
    std::ofstream out(*ctx.root / "data" / "owner_class_overrides.json");
    if (!out) return;
    json oj = json::object();
    for (const auto& kv : *ctx.owner_class_overrides) oj[kv.first] = kv.second;
    out << oj.dump(2);
}

void markOwnerAggregatesDirtyIfLayerSizesChanged(const OwnerAggregatesContext& ctx) {
    if (!ctx.layers || !ctx.owner_aggregates_dirty || !ctx.owner_cached_parcel_size || !ctx.owner_cached_real_property_size) return;
    if (ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
        const size_t parcel_size = (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size();
        if (parcel_size != *ctx.owner_cached_parcel_size) {
            *ctx.owner_cached_parcel_size = parcel_size;
            *ctx.owner_aggregates_dirty = true;
        }
    }
    if (ctx.real_property_layer_idx >= 0 && (size_t)ctx.real_property_layer_idx < ctx.layers->size()) {
        const size_t real_property_size = (*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.size();
        if (real_property_size != *ctx.owner_cached_real_property_size) {
            *ctx.owner_cached_real_property_size = real_property_size;
            *ctx.owner_aggregates_dirty = true;
        }
    }
}

void rebuildOwnerAggregates(const OwnerAggregatesContext& ctx) {
    if (!ctx.owner_aggregates_dirty || !ctx.owner_aggregates || !ctx.selected_owners ||
        !ctx.owner_class_overrides || !ctx.unified_parcels || !ctx.layers || !ctx.owner_sorted_mode) {
        return;
    }
    if (!*ctx.owner_aggregates_dirty) return;
    if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return;

    const auto owner_prof_begin = std::chrono::steady_clock::now();
    std::unordered_map<std::string, OwnerAggregate> acc;
    const auto& parcels = (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features;
    const bool parcel_data_ready = !parcels.empty();
    const bool real_property_data_ready =
        ctx.real_property_layer_idx >= 0 &&
        (size_t)ctx.real_property_layer_idx < ctx.layers->size() &&
        !(*ctx.layers)[(size_t)ctx.real_property_layer_idx].features.empty();

    if (!parcel_data_ready) {
        *ctx.owner_sorted_mode = -1;
    } else {
        for (const auto& parcel_record : *ctx.unified_parcels) {
            const LayerDef::FeatureGeom* pf = parcel_record.parcel_geom;
            if (!pf) continue;
            std::string owner = parcel_record.owner;
            if (owner.empty()) continue;
            auto& row = acc[owner];
            if (row.owner.empty()) row.owner = owner;
            if (row.owner_class.empty()) {
                auto oit = ctx.owner_class_overrides->find(owner);
                row.owner_class = oit != ctx.owner_class_overrides->end() ? oit->second : classifyOwner(owner);
            }
            row.property_count += 1;
            row.area_m2 += parcelAreaSqM(*pf);
            row.value_usd += parcel_record.current_value;
        }
        ctx.owner_aggregates->clear();
        ctx.owner_aggregates->reserve(acc.size());
        for (auto& kv : acc) ctx.owner_aggregates->push_back(std::move(kv.second));
        if (!ctx.selected_owners->empty()) {
            std::unordered_set<std::string> owners_present;
            owners_present.reserve(ctx.owner_aggregates->size());
            for (const auto& row : *ctx.owner_aggregates) owners_present.insert(row.owner);
            for (auto it = ctx.selected_owners->begin(); it != ctx.selected_owners->end();) {
                if (owners_present.find(*it) == owners_present.end()) it = ctx.selected_owners->erase(it);
                else ++it;
            }
        }
        *ctx.owner_aggregates_dirty = !real_property_data_ready && ctx.owner_aggregates->empty();
        *ctx.owner_sorted_mode = -1;
    }

    if (ctx.prof_owner_ms_last) {
        ctx.prof_owner_ms_last->store(profMsSince(owner_prof_begin), std::memory_order_relaxed);
    }
}

void refreshFilteredAggregateSnapshot(const OwnerAggregatesContext& ctx) {
    if (!ctx.filtered_aggregate_snapshot || !ctx.selected_owners || !ctx.owner_aggregates || !ctx.unified_parcels) return;

    if (!ctx.selected_owners->empty()) {
        std::vector<std::string> selected_owner_list(ctx.selected_owners->begin(), ctx.selected_owners->end());
        std::sort(selected_owner_list.begin(), selected_owner_list.end());
        uint64_t selection_key = 1469598103934665603ULL;
        for (const auto& owner : selected_owner_list) {
            selection_key ^= (uint64_t)owner.size();
            selection_key *= 1099511628211ULL;
            for (unsigned char ch : owner) {
                selection_key ^= (uint64_t)ch;
                selection_key *= 1099511628211ULL;
            }
        }
        uint64_t data_key = 1469598103934665603ULL;
        data_key ^= (uint64_t)ctx.owner_aggregates->size(); data_key *= 1099511628211ULL;
        data_key ^= (uint64_t)*ctx.owner_cached_parcel_size; data_key *= 1099511628211ULL;
        data_key ^= (uint64_t)*ctx.owner_cached_real_property_size; data_key *= 1099511628211ULL;
        data_key ^= (uint64_t)(ctx.parcel_vacancy_generation_applied + 3); data_key *= 1099511628211ULL;
        data_key ^= (uint64_t)(ctx.parcel_tax_generation_applied + 7); data_key *= 1099511628211ULL;
        if (!ctx.filtered_aggregate_snapshot->valid ||
            ctx.filtered_aggregate_snapshot->selection_key != selection_key ||
            ctx.filtered_aggregate_snapshot->data_key != data_key) {
            *ctx.filtered_aggregate_snapshot = {};
            ctx.filtered_aggregate_snapshot->valid = true;
            ctx.filtered_aggregate_snapshot->selection_key = selection_key;
            ctx.filtered_aggregate_snapshot->data_key = data_key;
            for (const auto& row : *ctx.owner_aggregates) {
                if (ctx.selected_owners->find(row.owner) == ctx.selected_owners->end()) continue;
                ctx.filtered_aggregate_snapshot->owner_property_count += row.property_count;
                ctx.filtered_aggregate_snapshot->owner_area_m2 += row.area_m2;
                ctx.filtered_aggregate_snapshot->owner_value_usd += row.value_usd;
            }
            if (ctx.parcel_layer_idx >= 0 && ctx.layers && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
                for (const auto& parcel_record : *ctx.unified_parcels) {
                    std::string owner = parcel_record.owner;
                    if (owner.empty() || ctx.selected_owners->find(owner) == ctx.selected_owners->end()) continue;
                    const int vac_notice = parcel_record.vacant_notice_count;
                    const int vac_rehab = parcel_record.vacant_rehab_count;
                    if ((vac_notice + vac_rehab) <= 0) continue;
                    ctx.filtered_aggregate_snapshot->vacancy_parcels_matched++;
                    if (parcel_record.parcel_geom && !parcel_record.parcel_geom->rings.empty()) {
                        ctx.filtered_aggregate_snapshot->vacancy_parcels_with_geometry++;
                    }
                }
            }
        }
    } else {
        *ctx.filtered_aggregate_snapshot = {};
    }
}
}

const std::vector<std::pair<std::string, std::string>>& ownerClassItems() {
    static const std::vector<std::pair<std::string, std::string>> kItems = {
        {"all", "All"},
        {"government", "Government"},
        {"individual", "Individual"},
        {"receivership", "Receivership"},
        {"development_corp", "Development Corp"},
        {"nonprofit", "Nonprofit"},
        {"trust_or_estate", "Trust/Estate"},
        {"financial_institution", "Financial Institution"},
        {"llc_corporate", "LLC/Corporate"},
        {"unknown", "Unknown"},
    };
    return kItems;
}

void syncOwnerAggregates(const OwnerAggregatesContext& ctx) {
    if (!ctx.root || !ctx.layers || !ctx.unified_parcels || !ctx.selected_owners ||
        !ctx.owner_class_overrides || !ctx.owner_aggregates || !ctx.filtered_aggregate_snapshot ||
        !ctx.owner_aggregates_dirty || !ctx.owner_sorted_mode || !ctx.owner_cached_parcel_size ||
        !ctx.owner_cached_real_property_size) {
        return;
    }

    loadOwnerClassOverrides(ctx);
    persistOwnerClassOverrides(ctx);
    markOwnerAggregatesDirtyIfLayerSizesChanged(ctx);
    rebuildOwnerAggregates(ctx);
    refreshFilteredAggregateSnapshot(ctx);
}
