# PerfCI Profiling & GPU Frame Capture Module

Opt-in PerfCI protocol v1 client for iOS, macOS, and Android profiling builds, with Metal present-boundary `.gputrace` capture on Apple platforms.

## Build

This module is disabled by default (`is_enabled() -> False`), so standard builds contain zero PerfCI code or symbols. Pass `module_perfci_enabled=yes` to SCons to define `MODULE_PERFCI_ENABLED` and compile this module:

```sh
# iOS (Metal + .gputrace capture)
scons platform=ios target=template_release arch=arm64 metal=yes vulkan=no opengl3=yes module_perfci_enabled=yes

# Android (Vulkan + Perfetto / RenderDoc Remote / heapprofd)
scons platform=android target=template_release arch=arm64 vulkan=yes opengl3=yes module_perfci_enabled=yes
```

On iOS, the exported application's `Info.plist` must include `<key>MetalCaptureEnabled</key><true/>`, and the process must be launched with `MTL_CAPTURE_ENABLED=1`. On non-root Android, package into a `debuggable` APK container so Perfetto, RenderDoc Vulkan Layer, and `heapprofd` can attach.

## Runtime Behavior

- **Activation gate**: Remains inactive unless `PERFCI_PORT` is set in the environment or (on Android) `<ExternalFilesDir>/perfci/perfci_bootstrap.json` exists (deleted immediately after reading so manual launches do not reconnect).
- **Capabilities**: Declares `screenshot` and `render_metrics` on all platforms, plus `gpu_frame_capture` when the active rendering driver is `metal`.
- **Metal frame-boundary capture**: On `capture_gpu_frame`, `RenderingDeviceDriverMetal` starts `MTLCaptureManager` (`MTLCaptureDestinationGPUTraceDocument`) using the driver's own `MTL::Device` right after the preceding frame's swapchain present (before the next frame creates its first `MTLCommandBuffer`) and calls `stopCapture` right after the captured frame commits its last swapchain `MTLCommandBuffer`, replying with `{"saved_path": "<output_name>", "frame": <rendered_frame>}` once finalized on disk.
