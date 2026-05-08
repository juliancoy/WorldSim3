#pragma once

#include "types.h"

#include <string>

std::string getPropertyValue(const LayerDef::FeatureGeom& fg, const std::string& key);
std::string normalizeJoinKey(std::string s);
std::string zoningClassKey(const LayerDef::FeatureGeom& fg);
std::string zoningGroupKey(const std::string& zone_key);
std::string zoningClassLabel(const LayerDef::FeatureGeom& fg);
std::string zoningClassTooltip(const LayerDef::FeatureGeom& fg);
ImVec4 colorFromStableKey(const std::string& key);
ImVec4 zoningColorFromConvention(const std::string& zone_key);
ImVec4 zoningShadeVariant(const ImVec4& base_color, const std::string& zone_key);
