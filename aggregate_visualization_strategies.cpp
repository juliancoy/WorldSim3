#include "aggregate_visualization_strategies.h"

bool isHeatmapAggregateMethod(int aggregate_algo) {
    return aggregate_algo >= kAggregateGridBinning && aggregate_algo <= kAggregateMedianChoropleth;
}

bool isSmoothHeatmapAggregateMethod(int aggregate_algo) {
    return aggregate_algo == kAggregateKdeGaussian ||
           aggregate_algo == kAggregateGpuSplatBlur ||
           aggregate_algo == kAggregateMultiResPyramid;
}

const char* aggregateStrategyName(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateGridBinning: return "Grid Binning";
        case kAggregateKdeGaussian: return "KDE (Gaussian)";
        case kAggregateGpuSplatBlur: return "GPU Splat + Blur";
        case kAggregateHexBinning: return "Hex Binning";
        case kAggregateMultiResPyramid: return "Multi-res Pyramid";
        case kAggregateLodGeometry: return "LOD Geometry";
        case kAggregateMedianChoropleth: return "Median Choropleth";
        default: return "Unknown";
    }
}

int aggregateAlgoFromLayerUiIndex(int ui_index) {
    switch (ui_index) {
        case 1: return kAggregateKdeGaussian;
        case 2: return kAggregateGpuSplatBlur;
        case 3: return kAggregateLodGeometry;
        case 4: return kAggregateHexBinning;
        case 5: return kAggregateMultiResPyramid;
        case 6: return kAggregateMedianChoropleth;
        default: return kAggregateHexBinning;
    }
}

int aggregateLayerUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateKdeGaussian: return 1;
        case kAggregateGpuSplatBlur: return 2;
        case kAggregateLodGeometry: return 3;
        case kAggregateHexBinning:
        case kAggregateGridBinning: return 4;
        case kAggregateMultiResPyramid: return 5;
        case kAggregateMedianChoropleth: return 6;
        default: return 4;
    }
}

int aggregateAlgoFromGlobalUiIndex(int ui_index) {
    switch (ui_index) {
        case 0: return kAggregateKdeGaussian;
        case 1: return kAggregateGpuSplatBlur;
        case 2: return kAggregateLodGeometry;
        case 3: return kAggregateHexBinning;
        case 4: return kAggregateMultiResPyramid;
        case 5: return kAggregateMedianChoropleth;
        default: return kAggregateHexBinning;
    }
}

int aggregateGlobalUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateKdeGaussian: return 0;
        case kAggregateGpuSplatBlur: return 1;
        case kAggregateLodGeometry: return 2;
        case kAggregateHexBinning:
        case kAggregateGridBinning: return 3;
        case kAggregateMultiResPyramid: return 4;
        case kAggregateMedianChoropleth: return 5;
        default: return 3;
    }
}
