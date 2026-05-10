#pragma once

#include "dataset_library.h"
#include "types.h"

#include <filesystem>

bool layerHasImportSource(const LayerDef& layer);
VersionedDownloadResult downloadOrImportLayer(
    const LayerDef& layer,
    const std::filesystem::path& out_path,
    const std::filesystem::path& root);
