#include "app_background_services.h"

void startAppBackgroundServices(
    const AppBackgroundServicesBootstrapInput& input,
    AppBackgroundServices& out) {
    StatusApiContextFactoryInput status_api = input.status_api;
    status_api.hover_debug_state = &out.hover_debug_state;
    out.status_api_worker = startStatusApiWorker(makeStatusApiContext(status_api));

    DatasetLanApiContext dataset_ctx{
        input.app_version,
        input.protocol_version,
        *input.root,
        input.stop,
        &out.p2p_mutex,
        &out.p2p_mailbox
    };
    out.dataset_api_worker = startDatasetApiWorker(dataset_ctx);
    out.lan_discovery_worker = startLanDiscoveryWorker(dataset_ctx);
}
