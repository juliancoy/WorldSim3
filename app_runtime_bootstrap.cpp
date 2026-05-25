#include "app_runtime_bootstrap.h"

#include "app_utils.h"
#include "cache_io.h"

#include <array>

namespace fs = std::filesystem;

namespace {
constexpr std::array<const char*, 24> kParcelJurisdictionOptions = {
        "Allegany County",
        "Anne Arundel County",
        "Baltimore City",
        "Baltimore County",
        "Calvert County",
        "Caroline County",
        "Carroll County",
        "Cecil County",
        "Charles County",
        "Dorchester County",
        "Frederick County",
        "Garrett County",
        "Harford County",
        "Howard County",
        "Kent County",
        "Montgomery County",
        "Prince George's County",
        "Queen Anne's County",
        "St. Mary's County",
        "Somerset County",
        "Talbot County",
        "Washington County",
        "Wicomico County",
        "Worcester County"
    };

GeometryArtifactClass startupGeometryClassForLayer(const LayerDef& layer, bool is_parcel_layer) {
    if (is_parcel_layer) return GeometryArtifactClass::Polygon;
    if (layerUsesPointGeometry(layer)) return GeometryArtifactClass::Point;
    if (layerUsesPolylineGeometry(layer)) return GeometryArtifactClass::Polyline;
    return GeometryArtifactClass::Polygon;
}

}  // namespace

const std::array<const char*, 24>& defaultParcelJurisdictionOptions() {
    return kParcelJurisdictionOptions;
}

void seedDefaultParcelJurisdictions(ParcelJurisdictionFilterState& state) {
    for (const char* jurisdiction : kParcelJurisdictionOptions) {
        state.selected_jurisdictions.insert(jurisdiction);
    }
}

void initializeLayerStateFromPersistedArtifacts(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int parcel_layer_idx,
    std::mutex& status_mutex,
    std::vector<LayerRuntimeState>& layer_states,
    size_t idx) {
    if (idx >= layers.size() || idx >= layer_states.size()) return;
    const fs::path layer_path = resolveStoredLayerPath(root, layers[idx]);
    std::string sig;
    std::string resolved_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &resolved_kind)) {
        std::lock_guard<std::mutex> lk(status_mutex);
        layer_states[idx].status = LayerPipelineStatus::Failed;
        layer_states[idx].feature_count = 0;
        layer_states[idx].error = "canonical metadata/signature missing";
        layer_states[idx].hydration_source_signature.clear();
        layer_states[idx].hydration_source_kind.clear();
        layer_states[idx].hydration_phase = "metadata_missing";
        return;
    }
    const bool is_primary_parcel_layer = static_cast<int>(idx) == parcel_layer_idx;
    const GeometryArtifactClass cls = startupGeometryClassForLayer(layers[idx], is_primary_parcel_layer);
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, layers[idx].file, cls);

    bool artifact_ok = false;
    size_t artifact_features = 0;
    if (cls == GeometryArtifactClass::Point) {
        PointGeometryArtifact artifact;
        artifact_ok = loadBinaryPointGeometryArtifact(artifact_path, sig, artifact);
        artifact_features = artifact.features.size();
    } else if (cls == GeometryArtifactClass::Polyline) {
        PolylineGeometryArtifact artifact;
        artifact_ok = loadBinaryPolylineGeometryArtifact(artifact_path, sig, artifact);
        artifact_features = artifact.features.size();
    } else if (cls == GeometryArtifactClass::Polygon) {
        PolygonGeometryArtifact artifact;
        artifact_ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact);
        artifact_features = artifact.features.size();
    }

    std::lock_guard<std::mutex> lk(status_mutex);
    layer_states[idx].hydration_source_signature = sig;
    layer_states[idx].hydration_source_kind = resolved_kind;
    layer_states[idx].hydration_loaded_from_cache = true;
    layer_states[idx].geometry_artifact_class = cls;
    layer_states[idx].geometry_artifact_path = artifact_path.string();
    layer_states[idx].geometry_source_signature = artifact_ok ? sig : std::string{};
    layer_states[idx].geometry_loaded_from_artifact = artifact_ok;
    layer_states[idx].geometry_phase = artifact_ok ? "artifact_ready" : "artifact_missing";
    layer_states[idx].feature_count = artifact_features;
    if (artifact_ok) {
        layer_states[idx].status = LayerPipelineStatus::Ready;
        layer_states[idx].hydration_phase = "metadata_only_geometry_artifact";
        layer_states[idx].error.clear();
    } else {
        layer_states[idx].status = LayerPipelineStatus::Queued;
        layer_states[idx].hydration_phase = "metadata_only_artifact_missing";
        layer_states[idx].error = "compiled geometry artifact missing or stale";
    }
}

void initializeEnabledLayerStatesFromPersistedArtifacts(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    int parcel_layer_idx,
    std::mutex& status_mutex,
    std::vector<LayerRuntimeState>& layer_states) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].enabled) continue;
        initializeLayerStateFromPersistedArtifacts(root, layers, parcel_layer_idx, status_mutex, layer_states, i);
    }
}

size_t countReadyLayerStates(std::mutex& status_mutex, const std::vector<LayerRuntimeState>& layer_states) {
    std::lock_guard<std::mutex> lk(status_mutex);
    size_t ready_count = 0;
    for (const auto& state : layer_states) {
        if (state.status == LayerPipelineStatus::Ready) ++ready_count;
    }
    return ready_count;
}
