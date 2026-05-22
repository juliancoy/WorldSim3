#include "aggregate_visualization_strategies.h"

bool isHeatmapAggregateMethod(int aggregate_algo) {
    return aggregate_algo >= kAggregateGridBinning && aggregate_algo <= kAggregateMedianChoropleth;
}

bool isSmoothHeatmapAggregateMethod(int aggregate_algo) {
    return aggregate_algo == kAggregateKdeGaussian ||
           aggregate_algo == kAggregateGpuSplatBlur ||
           aggregate_algo == kAggregateGpuSplatHue ||
           aggregate_algo == kAggregateMultiResPyramid;
}

bool isPointClusterAggregateMethod(int aggregate_algo) {
    return aggregate_algo == kAggregatePointClustering;
}

const char* aggregateStrategyName(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateNone: return "None";
        case kAggregateGridBinning: return "Grid Binning";
        case kAggregateKdeGaussian: return "KDE (Gaussian)";
        case kAggregateGpuSplatBlur: return "GPU Splat + Blur";
        case kAggregateGpuSplatHue: return "GPU Splat Hue";
        case kAggregateHexBinning: return "Hex Binning";
        case kAggregateMultiResPyramid: return "Multi-res Pyramid";
        case kAggregateLodGeometry: return "LOD Geometry";
        case kAggregateMedianChoropleth: return "Median Choropleth";
        case kAggregatePointClustering: return "Point Clustering";
        default: return "Unknown";
    }
}

int aggregateAlgoFromLayerUiIndex(int ui_index) {
    switch (ui_index) {
        case 1: return kAggregateKdeGaussian;
        case 2: return kAggregateGpuSplatBlur;
        case 3: return kAggregateGpuSplatHue;
        case 4: return kAggregateLodGeometry;
        case 5: return kAggregateHexBinning;
        case 6: return kAggregateMultiResPyramid;
        case 7: return kAggregateMedianChoropleth;
        case 8: return kAggregatePointClustering;
        default: return kAggregateNone;
    }
}

int aggregateLayerUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateNone: return 0;
        case kAggregateKdeGaussian: return 1;
        case kAggregateGpuSplatBlur: return 2;
        case kAggregateGpuSplatHue: return 3;
        case kAggregateLodGeometry: return 4;
        case kAggregateHexBinning:
        case kAggregateGridBinning: return 5;
        case kAggregateMultiResPyramid: return 6;
        case kAggregateMedianChoropleth: return 7;
        case kAggregatePointClustering: return 8;
        default: return 0;
    }
}

int aggregateAlgoFromGlobalUiIndex(int ui_index) {
    switch (ui_index) {
        case 1: return kAggregateKdeGaussian;
        case 2: return kAggregateGpuSplatBlur;
        case 3: return kAggregateGpuSplatHue;
        case 4: return kAggregateLodGeometry;
        case 5: return kAggregateHexBinning;
        case 6: return kAggregateMultiResPyramid;
        case 7: return kAggregateMedianChoropleth;
        default: return kAggregateNone;
    }
}

int aggregateGlobalUiIndexFromAlgo(int aggregate_algo) {
    switch (aggregate_algo) {
        case kAggregateNone: return 0;
        case kAggregateKdeGaussian: return 1;
        case kAggregateGpuSplatBlur: return 2;
        case kAggregateGpuSplatHue: return 3;
        case kAggregateLodGeometry: return 4;
        case kAggregateHexBinning:
        case kAggregateGridBinning: return 5;
        case kAggregateMultiResPyramid: return 6;
        case kAggregateMedianChoropleth: return 7;
        default: return 0;
    }
}
