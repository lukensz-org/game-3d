# game-3d

Independent 3D game consumer of the installed `engine` CMake Config package.

This repository creates a real Platform window, uses the public `GraphicsContext` facade for ordinary windowed Vulkan bootstrap and the `0.5.0` ordinary frame protocol, and draws multiple simple colored 3D cubes under perspective projection with public depth-tested hidden-surface removal. Game-owned shaders, shader compilation, transform math, camera/scene data, depth resources, projection/aspect policy, workload, and game-loop policy stay in this repository. It does not introduce a speculative Renderer3D abstraction.

## Engine requirement

This consumer is pinned to:

- engine checkpoint: `deeb4c551af89bc0fb683b2a20b76a9cc9e9e43c`
- engine CMake package: `0.5.0` exact
- engine package component: `graphics`

There is no annotated `v0.5.0` tag required by this consumer. Package version `0.5.0 EXACT` is not source provenance by itself; the consumed install must be produced from exact engine checkpoint `deeb4c551af89bc0fb683b2a20b76a9cc9e9e43c`, which is the developer-locally validated frame-protocol implementation checkpoint. This project does not follow `main`, `master`, or any other moving engine branch.

The CMake request is:

```cmake
find_package(
    engine 0.5.0 EXACT CONFIG REQUIRED
    COMPONENTS graphics
)
```

The executable links the documented ordinary game target boundary:

- `engine::platform`
- `engine::vulkan_graphics`

Upgrades are explicit consumer changes to a later validated engine package/provenance identity. They are never automatic.

## Prerequisite

Install the exact engine checkpoint above to a prefix you control. This repository does not vendor engine source, add the engine as a subdirectory or submodule, or fetch engine source through CMake.

Supply that prefix through normal CMake package search input such as `CMAKE_PREFIX_PATH` or `engine_DIR`. Do not commit a machine-specific absolute prefix.

The installed `graphics` component requires:

- a CMake-resolvable Vulkan 1.3+ development package;
- a CMake-resolvable GLFW package (`glfw3` >= 3.5.1);
- a local `glslc` on `PATH` for consumer-owned shader compilation.

Shader tooling is a `game-3d` build requirement. It is not exported by the installed engine package.

For Debug Khronos validation evidence, consume an engine package built in Debug so the facade's owned Runtime retains validation-enabled behavior.

## Configure, build, and run

From a developer shell that can already resolve Vulkan, GLFW, `glslc`, and the installed engine prefix:

```bash
cmake --preset default -DCMAKE_PREFIX_PATH=/path/to/engine-prefix
cmake --build --preset default
./build/game_3d --bounded
./build/game_3d
```

`--bounded` creates the normal rendering path, presents four successfully accepted frames, and exits successfully without interaction. Omit it for interactive execution.

The consumer target does not set `CXX_STANDARD` or request `cxx_std_23`. Effective C++23 compilation comes from the imported engine usage requirements.

## GraphicsContext boundary

`engine::graphics::vulkan::GraphicsContext` is constructed from the caller-owned `engine::platform::WindowSystem` and `Window`. It owns the common Runtime, Surface, device, allocator, execution, swapchain, RenderingContext, and `SwapchainLifecycle` bootstrap/teardown chain.

Ordinary frames use only the game-facing `0.5.0` protocol:

- `beginFrame(window.framebufferExtent())` coordinates presentation extent synchronization/recreation, normal retired-generation reclamation, and image acquisition;
- `FrameBeginResult` exposes the ready generation/image identity, active extent/color format, generation change, reclaimed-generation IDs, and deferred/try-again/surface-lost outcomes;
- every ready begin is paired with exactly one `presentFrame(...)` before another begin;
- `presentFrame(...)` performs the render/present step and exposes accepted/try-again/surface-lost plus GPU submission completion evidence;
- zero/minimized framebuffer extents are passed into `beginFrame(...)`, and `deferred` is handled with consumer-owned sleep/backoff;
- `waitForSubmittedWork()` is used only when caller-owned context-backed graphics/depth state must be replaced safely or at shutdown.

The consumer does not reconstruct the lower lifecycle with `sync(...)`, `reclaimRetiredGenerations()`, `acquire()`, or `renderAndPresent(...)`, does not directly construct lower Vulkan bootstrap objects or `SwapchainLifecycle`, and does not use raw `vkDeviceWaitIdle` shutdown plumbing.

## Workload and hidden-surface acceptance

Cubes are generated from `gl_VertexIndex` (36 vertices) with per-draw push-constant MVP + color. Projection aspect is derived from the extent reported by each ready `FrameBeginResult`.

Extent-dependent 3D state remains consumer-owned: an extent-matched Resources depth image plus `ResourceDepthTarget`, and projection aspect. The depth format is selected through the public `GraphicsContext` capability surface. The depth image is created through the non-owning allocator reference returned by `GraphicsContext::resources()`, while the depth target is created through the facade. Zero-sized depth resources are never created because only ready frames establish depth state.

The caller-owned depth-enabled `GraphicsState` is compatible with the ready frame's color format and the selected depth format. It is created or replaced only when that format compatibility requires replacement; a swapchain generation change by itself does not force pipeline replacement. When existing context-backed depth or graphics objects need replacement, the consumer first waits for prior submitted Rendering work. The old depth target is destroyed before the depth image it references, and all context-backed consumer objects are destroyed before the `GraphicsContext`.

The accepted scene places overlapping cubes at different view-space depths. Draws are issued near-to-far so the probe is depth-adversarial: without depth testing/writing, later farther draws would incorrectly overwrite nearer geometry where they overlap; with a working public depth path, nearer geometry must still occlude farther geometry. Hidden-surface removal uses the public Rendering depth contract: capability-selected depth format, a Resources-owned sample-count-1 depth image for the active extent, `ResourceDepthTarget`, depth-enabled graphics state with `VK_COMPARE_OP_LESS`, and `ColorPass` depth clear/attachment state.

Bounded frame accounting advances only on `FrameEndDisposition::accepted`; begin-frame retry/deferred outcomes and present try-again outcomes do not count as completed frames. Surface loss remains a fatal observable consumer condition. Interactive resize/recreation keeps depth resources extent-matched and projection aspect current, including minimize/zero-extent deferral and recovery without creating zero-sized depth resources.
