## Layer GPU Buffer Status

The current GPU aggregate path does not create independently owned GPU buffers per layer.
Instead, it uses one shared global Vulkan context with persistent reusable buffers:

- `sample_buf` for packed input samples
- `accum_buf` for aggregate output/accumulation
- one shared descriptor set
- one shared command buffer

Those buffers are dynamically resized to fit the current aggregate request, so sizing is data-driven at runtime. However, ownership is global to the aggregate subsystem, not per-layer and not per-cache-key.

## What This Means

The statement "all layer GPU buffers are created independently and dynamically according to Vulkan best practices" is not accurate for the current implementation.

What is true:

- buffer sizes are adjusted dynamically based on request size
- persistent buffer reuse avoids per-dispatch allocation churn
- the compute path is functional and reusable across requests

What is not true:

- buffers are not independently created per layer
- buffers are not isolated per aggregate key or per cached layer result
- the path does not follow ideal Vulkan performance practice for heavy GPU workloads

## Vulkan Best-Practice Gaps

The current implementation still has several simplifications that make it weaker than a best-practice Vulkan design:

- it uses host-visible/coherent buffers directly instead of preferring device-local buffers with staging/readback where appropriate
- it updates one shared descriptor set instead of using cleaner resource ownership boundaries
- it reuses one global command buffer for all aggregate work
- it blocks after each dispatch with `vkQueueWaitIdle`, which introduces a full-queue stall
- it reads aggregate results back to the CPU instead of keeping results resident on the GPU for direct texture consumption

## Better End State

A stronger Vulkan design would move toward:

- per-layer or per-aggregate-key resource ownership
- device-local working buffers and images
- explicit staging/upload/readback paths only when needed
- fence-based or timeline-semaphore-based synchronization instead of `vkQueueWaitIdle`
- GPU-resident output textures cached across frames

## Relevant Code

- `heatmap_gpu_aggregate.cpp`
- `heatmap_render.cpp`
- `AGREGATE_VULKAN.md`
