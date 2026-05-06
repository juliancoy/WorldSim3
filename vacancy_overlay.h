#pragma once

#include "types.h"

#include <filesystem>
#include <vector>

void saveDerivedVacancyStatus(
    const std::filesystem::path& out_path,
    const std::vector<LayerDef::FeatureGeom>& parcel_features,
    const std::vector<int>& notice_counts,
    const std::vector<int>& rehab_counts,
    size_t vacant_notice_rows_total,
    size_t vacant_rehab_rows_total,
    size_t vacant_notice_rows_matched,
    size_t vacant_rehab_rows_matched);

int runVacancySelftest(const std::filesystem::path& root);
