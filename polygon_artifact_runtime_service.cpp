#include "polygon_artifact_runtime_service.h"

#include "app_utils.h"
#include "worldsim_app.h"

void syncPolygonGeometryArtifacts(
    const PolygonArtifactRuntimeSyncInput& input,
    PolygonArtifactRuntimeState& state) {
    if (!input.root || !input.layers || !input.layer_states || !input.gpu_uploaded_signatures) {
        return;
    }

    for (size_t li = 0; li < input.layers->size() && li < input.layer_states->size(); ++li) {
        const LayerDef& layer = (*input.layers)[li];
        LayerRuntimeState& layer_state = (*input.layer_states)[li];
        const bool parcel_layer = static_cast<int>(li) == input.parcel_layer_idx;
        if (layerUsesPointGeometry(layer) ||
            layerUsesPolylineGeometry(layer) ||
            !layer.enabled ||
            layer_state.status != LayerPipelineStatus::Ready ||
            layer_state.hydration_source_signature.empty() ||
            parcel_layer) {
            state.geometry_artifacts.erase(li);
            state.artifact_signatures.erase(li);
            if (layer_state.geometry_artifact_class == GeometryArtifactClass::Polygon && !parcel_layer) {
                layer_state.geometry_source_signature.clear();
                layer_state.geometry_phase.clear();
                layer_state.geometry_loaded_from_artifact = false;
                layer_state.geometry_gpu_resident = false;
            }
            continue;
        }

        const std::filesystem::path artifact_path =
            geometryArtifactCachePathForLayerFile(*input.root, layer.file, GeometryArtifactClass::Polygon);
        layer_state.geometry_artifact_class = GeometryArtifactClass::Polygon;
        layer_state.geometry_artifact_path = artifact_path.string();
        const std::string& sig = layer_state.hydration_source_signature;
        const bool gpu_resident = input.gpu_uploaded_signatures->find(li) != input.gpu_uploaded_signatures->end();

        auto sig_it = state.artifact_signatures.find(li);
        if (sig_it != state.artifact_signatures.end() &&
            sig_it->second == sig &&
            state.geometry_artifacts.find(li) != state.geometry_artifacts.end()) {
            layer_state.geometry_source_signature = sig;
            layer_state.geometry_phase = "artifact_validated";
            layer_state.geometry_loaded_from_artifact = true;
            layer_state.geometry_gpu_resident = gpu_resident;
            continue;
        }

        PolygonGeometryArtifact artifact;
        if (!loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact) ||
            (!layer.features.empty() && artifact.features.size() != layer.features.size())) {
            state.geometry_artifacts.erase(li);
            state.artifact_signatures.erase(li);
            layer_state.geometry_source_signature = sig;
            layer_state.geometry_phase = "artifact_missing";
            layer_state.geometry_loaded_from_artifact = false;
            layer_state.geometry_gpu_resident = false;
            continue;
        }

        state.geometry_artifacts[li] = std::move(artifact);
        state.artifact_signatures[li] = sig;
        layer_state.geometry_source_signature = sig;
        layer_state.geometry_phase = gpu_resident ? "gpu_ready" : "artifact_validated";
        layer_state.geometry_loaded_from_artifact = true;
        layer_state.geometry_gpu_resident = gpu_resident;
    }
}
