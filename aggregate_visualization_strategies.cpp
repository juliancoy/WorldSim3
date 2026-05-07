#include "aggregate_visualization_strategies.h"

bool isHeatmapAggregateMethod(int aggregate_algo) {
    return aggregate_algo >= kAggregateGridBinning && aggregate_algo <= kAggregateMultiResPyramid;
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
        default: return "Unknown";
    }
}

int aggregateAlgoFromLayerUiIndex(int ui_index) {
    switch (ui_index) {
        case 1: return kAggregateGridBinning;
        case 2: return kAggregateKdeGaussian;
        case 3: return kAggregateGpuSplatBlur;
        case 4: return kAggregateLodGeometry;
        case 5: return kAggregateHexBinning;
        case 6: return kAggregateMultiResPyramid;
        default: return kAggregateGridBinning;
    }
}

int aggregateLayerUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateGridBinning: return 1;
        case kAggregateKdeGaussian: return 2;
        case kAggregateGpuSplatBlur: return 3;
        case kAggregateLodGeometry: return 4;
        case kAggregateHexBinning: return 5;
        case kAggregateMultiResPyramid: return 6;
        default: return 1;
    }
}

int aggregateAlgoFromGlobalUiIndex(int ui_index) {
    switch (ui_index) {
        case 0: return kAggregateGridBinning;
        case 1: return kAggregateKdeGaussian;
        case 2: return kAggregateGpuSplatBlur;
        case 3: return kAggregateLodGeometry;
        case 4: return kAggregateHexBinning;
        case 5: return kAggregateMultiResPyramid;
        default: return kAggregateGridBinning;
    }
}

int aggregateGlobalUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateGridBinning: return 0;
        case kAggregateKdeGaussian: return 1;
        case kAggregateGpuSplatBlur: return 2;
        case kAggregateLodGeometry: return 3;
        case kAggregateHexBinning: return 4;
        case kAggregateMultiResPyramid: return 5;
        default: return 0;
    }
}
