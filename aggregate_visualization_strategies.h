#pragma once

inline constexpr int kAggregateNone = -1;
inline constexpr int kAggregateGridBinning = 0;
inline constexpr int kAggregateKdeGaussian = 1;
inline constexpr int kAggregateGpuSplatBlur = 2;
inline constexpr int kAggregateGpuSplatHue = 3;
inline constexpr int kAggregateHexBinning = 4;
inline constexpr int kAggregateMultiResPyramid = 5;
inline constexpr int kAggregateLodGeometry = 6;
inline constexpr int kAggregateMedianChoropleth = 7;
inline constexpr int kAggregatePointClustering = 8;
inline constexpr int kParcelChoroplethMinZoom = 14;

bool isHeatmapAggregateMethod(int aggregate_algo);
bool isSmoothHeatmapAggregateMethod(int aggregate_algo);
bool isPointClusterAggregateMethod(int aggregate_algo);
const char* aggregateStrategyName(int aggregate_algo);
int aggregateAlgoFromLayerUiIndex(int ui_index);
int aggregateLayerUiIndexFromAlgo(int aggregate_algo);
int aggregateAlgoFromGlobalUiIndex(int ui_index);
int aggregateGlobalUiIndexFromAlgo(int aggregate_algo);
