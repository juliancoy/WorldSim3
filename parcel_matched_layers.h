#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

struct ParcelMatchedLayerBuildStat {
    std::string source;
    std::string output;
    size_t matched_parcels = 0;
    size_t source_events = 0;
    size_t unmatched_events = 0;
    size_t missing_keys = 0;
    bool written = false;
    bool skipped_up_to_date = false;
};

std::vector<ParcelMatchedLayerBuildStat> ensureParcelMatchedEventLayers(
    const std::filesystem::path& root,
    bool force,
    std::ostream* log);
