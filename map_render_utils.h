#pragma once

#include "imgui.h"
#include "types.h"

#include <cstdint>
#include <string>

bool tryGetFeaturePropertyFloat(const LayerDef::FeatureGeom& fg, const std::string& key, float& out);
ImVec4 heatColor(float t);
ImU32 colorWithAlpha(const ImVec4& c, int alpha);
ImVec4 darkenColor(const ImVec4& c, float mul);
ImVec4 blendVacancyColor(const ImVec4& notice_color, const ImVec4& rehab_color, int notice_count, int rehab_count);
ImVec4 blendTaxColor(
    const ImVec4& lien_color,
    const ImVec4& sale_color,
    bool lien_enabled,
    bool sale_enabled,
    int lien_count,
    int sale_count);
int overlayWeight(bool first_enabled, int first_count, bool second_enabled, int second_count);
int scaledOverlayAlpha(int base, int per_weight, int min_alpha, int max_alpha, int weight);
void hashCombineU64(uint64_t& seed, uint64_t v);
void hashCombineFloat(uint64_t& seed, float v);
void hashCombineQuantizedDouble(uint64_t& seed, double v, double scale);
