#pragma once

#include "imgui.h"
#include "types.h"

#include <string>
#include <unordered_map>
#include <vector>

struct EventSectorDef {
    std::string label;
    ImVec4 color;
    std::vector<std::string> matches;
};

const std::vector<EventSectorDef>& communitySectorDefs();
void ensureCommunitySectorFilterDefaults(std::unordered_map<std::string, bool>& enabled);
bool isCommunitySectorEventLayer(const LayerDef& layer);
std::string classifyCommunitySector(const LayerDef::FeatureRecord& fg);
