#include "worldsim_bootstrap.h"

WorldsimLayerIndices findWorldsimLayerIndices(const std::vector<LayerDef>& layers) {
    LayerRegistry registry(std::filesystem::current_path(), layers);
    return registry.indices();
}
