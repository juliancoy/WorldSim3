#pragma once

#include "types.h"

#include <filesystem>
#include <string>
#include <vector>

void saveDerivedVacancyStatus(
    const std::filesystem::path& out_path,
    const std::vector<LayerDef::FeatureRecord>& parcel_features,
    const std::vector<int>& notice_counts,
    const std::vector<int>& rehab_counts,
    size_t vacant_notice_rows_total,
    size_t vacant_rehab_rows_total,
    size_t vacant_notice_rows_matched,
    size_t vacant_rehab_rows_matched,
    const std::vector<std::string>* parcel_blocklot_by_feature = nullptr);

int runVacancySelftest(const std::filesystem::path& root);
