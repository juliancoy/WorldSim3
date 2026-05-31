#pragma once

#include "imgui.h"
#include "types.h"

#include <cstddef>
#include <string>

bool isRoadLabelLayer(const LayerDef& layer);
std::string roadLabelForFeatureProperties(const LayerDef::FeatureProperties& properties);
std::string roadLabelForFeature(const LayerDef& layer, size_t feature_idx);
float squaredDistancePointToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b, ImVec2* closest = nullptr);
