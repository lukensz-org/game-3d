# game-3d

Independent 3D game consumer of the installed `engine` CMake Config package.

This repository creates a real window, presents through the public engine WSI/Rendering contracts, and draws multiple simple colored 3D cubes under perspective projection with public depth-tested hidden-surface removal. Game-owned shaders, transform math, camera data, and shader compilation stay in this repository. It does not introduce a speculative Renderer3D abstraction.

## Engine requirement

This consumer is pinned to:

- engine Git tag: `v0.2.0`
- engine checkpoint: `9ef704e50cdd463212bbb61fae0f7cc96085a0d6`
- engine CMake package: `0.2.0` exact

The CMake request is:

```cmake
find_package(
    engine 0.2.0 EXACT CONFIG REQUIRED
    COMPONENTS vulkan wsi
)
```

Package version `0.2.0 EXACT` is not source provenance. The consumed install must be produced from the engine commit identified by annotated tag `v0.2.0` / checkpoint `9ef704e50cdd463212bbb61fae0f7cc96085a0d6`. This project does not follow `main`, `master`, or any other moving engine branch.

Upgrades are explicit consumer changes to a later engine release identity. They are never automatic.

## Prerequisite

Install the `v0.2.0` engine tree to a prefix you control. This repository does not vendor engine source, add the engine as a subdirectory or submodule, or fetch engine source through CMake.

Supply that prefix through normal CMake package search input such as `CMAKE_PREFIX_PATH` or `engine_DIR`. Do not commit a machine-specific absolute prefix.

The `vulkan` and `wsi` components require:

- a CMake-resolvable Vulkan 1.3+ development package;
- a CMake-resolvable GLFW package (`glfw3` >= 3.5.1);
- a local `glslc` on `PATH` for consumer-owned shader compilation.

Shader tooling is a `game-3d` build requirement. It is not exported by the installed engine package.

For Debug Khronos validation evidence, consume an engine package built in Debug so the installed Runtime retains validation-enabled behavior.

## Configure, build, and run

From a developer shell that can already resolve Vulkan, GLFW, `glslc`, and the installed engine prefix:

```bash
cmake --preset default -DCMAKE_PREFIX_PATH=/path/to/engine-prefix
cmake --build --preset default
./build/game_3d --bounded
./build/game_3d
```

`--bounded` creates the normal rendering path, presents four successful frames, and exits successfully without interaction. Omit it for interactive execution.

The consumer target does not set `CXX_STANDARD` or request `cxx_std_23`. Effective C++23 compilation comes from the imported engine usage requirements.

## Workload and hidden-surface acceptance

Cubes are generated from `gl_VertexIndex` (36 vertices) with per-draw push-constant MVP + color. Projection uses the active swapchain aspect ratio and updates after recreation.

The accepted scene places overlapping cubes at different view-space depths. Draws are issued far-to-near so correct nearer-over-farther occlusion cannot be attributed to overwrite order. Hidden-surface removal uses the public `v0.2.0` Rendering depth contract: capability-selected depth format, a Resources-owned sample-count-1 depth image for the active extent, `ResourceDepthTarget`, depth-enabled graphics state with nearer-wins comparison, and `ColorPass` depth clear/attachment state. Extent-dependent depth resources are recreated with the swapchain.
