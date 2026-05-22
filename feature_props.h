#pragma once

#include "types.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using FeaturePropertyPairs = std::vector<std::pair<std::string, std::string>>;

void rebuildFeaturePropertyRegistryForLayer(const LayerDef& layer);
void clearFeaturePropertyRegistryForLayer(const LayerDef& layer);
const LayerDef::FeatureProperties* getFeatureProperties(const LayerDef& layer, size_t feature_idx);
const FeaturePropertyPairs* getPropertyPairs(const LayerDef::FeatureRecord& fg);
const FeaturePropertyPairs* getTransientFeatureProperties(const LayerDef::FeatureRecord& fg);
void setTransientFeatureProperties(LayerDef::FeatureRecord& fg, FeaturePropertyPairs values);
void clearTransientFeatureProperties(const LayerDef::FeatureRecord& fg);
std::string getPropertyValue(const LayerDef::FeatureRecord& fg, const std::string& key);
std::string getPropertyValue(const LayerDef& layer, size_t feature_idx, const std::string& key);
std::string getFirstPropertyValue(const LayerDef::FeatureRecord& fg, std::initializer_list<const char*> keys);
std::string getFirstPropertyValue(const LayerDef& layer, size_t feature_idx, std::initializer_list<const char*> keys);
std::string normalizeJoinKey(std::string s);
std::string zoningClassKey(const LayerDef::FeatureRecord& fg);
std::string zoningClassKey(const LayerDef& layer, size_t feature_idx);
std::string zoningGroupKey(const std::string& zone_key);
std::string zoningClassLabel(const LayerDef::FeatureRecord& fg);
std::string zoningClassLabel(const LayerDef& layer, size_t feature_idx);
std::string zoningClassTooltip(const LayerDef::FeatureRecord& fg);
std::string zoningClassTooltip(const LayerDef& layer, size_t feature_idx);
ImVec4 colorFromStableKey(const std::string& key);
ImVec4 zoningColorFromConvention(const std::string& zone_key);
ImVec4 zoningShadeVariant(const ImVec4& base_color, const std::string& zone_key);
