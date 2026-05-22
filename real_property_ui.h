#pragma once

#include "types.h"

#include <string>

std::string normalizedRealPropertyOwnerName(const LayerDef::FeatureRecord* rp);
void drawRealPropertySummary(const LayerDef::FeatureRecord* rp, bool include_owner = true);
