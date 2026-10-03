# Vulkan testing branch — reserved, implementation deferred

`vulkan-testing` starts at the release 0.1 checkpoint. No Vulkan renderer, SDK
dependency, translation DLL or API switch has been added. The user's instruction
is to complete repository/release/backup setup before beginning renderer work.

The proposed future architecture keeps server/gameplay/client prediction at
**1,000 Hz** and renders independently, uncapped by default with console FPS caps.
The renderer should preserve stock assets, materials, UI, visual effects and restart
behavior, with bounded queued frames and explicit GPU resource lifetimes.

Future evaluation should compare camera-verified open scenes under the same input,
bot workload, resolution and settings. Measure CPU preparation, submission/Present,
GPU work, frame-time tails, latency, shader-cache warmup and the existing gameplay
gates. Vulkan does not automatically guarantee 100% GPU utilization or 1,000 FPS.
DX9 remains a comparison/fallback while a native Vulkan backend is developed.

A translation-layer experiment and a native backend are distinct projects. Native
work requires shader/material translation, image formats, descriptors, uploads,
pipeline caching, synchronization, swapchain/restart handling and visual regression
checks. No such experiment is authorized by the backup automation.

Source and workspace backups use `backup.ps1`; do not force-push or move `v0.1`.
Back up the final accepted Vulkan build before setting the readiness marker.
After Vulkan is actually accepted and readiness is explicitly confirmed, create
`docs/vulkan-ready.json` with `{ "ready": true }` to stop the ongoing backup loop,
or disable it through the app's scheduled-task controls.
