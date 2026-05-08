#include "model_tabs_panel.h"

#include "causal_panel_tab.h"
#include "change_log_tab.h"
#include "graph_model_tab.h"
#include "risk_scorecards_tab.h"
#include "scenarios_tab.h"
#include "spatial_index_tab.h"
#include "star_schema_tab.h"
#include "uncertainty_tab.h"

void drawVisualModelTabs(size_t layer_count) {
    drawGraphModelTab();
    drawStarSchemaTab();
    drawSpatialIndexTab(layer_count);
    drawUncertaintyTab();
    drawChangeLogTab();
    drawRiskScorecardsTab();
    drawCausalPanelTab();
    drawScenariosTab();
}
