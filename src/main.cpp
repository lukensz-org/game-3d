#include <engine/graphics/vulkan/rendering.hpp>
#include <engine/graphics/vulkan/runtime.hpp>
#include <engine/graphics/vulkan/wsi.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef GAME_3D_VERTEX_SPV
#error "GAME_3D_VERTEX_SPV must name the generated vertex SPIR-V file."
#endif

#ifndef GAME_3D_FRAGMENT_SPV
#error "GAME_3D_FRAGMENT_SPV must name the generated fragment SPIR-V file."
#endif

namespace {

using engine::graphics::vulkan::presentation::PixelExtent;
using engine::graphics::vulkan::presentation::SwapchainGeneration;
using engine::graphics::vulkan::rendering::ColorPass;
using engine::graphics::vulkan::rendering::DrawInfo;
using engine::graphics::vulkan::rendering::GraphicsState;
using engine::graphics::vulkan::rendering::PushConstantRange;
using engine::graphics::vulkan::rendering::PushConstantWrite;
using engine::graphics::vulkan::rendering::RenderingContext;
using engine::graphics::vulkan::rendering::ResourceDepthTarget;
using engine::graphics::vulkan::resources::Image;
using engine::graphics::vulkan::resources::ImageCreateInfo;
using engine::graphics::vulkan::resources::MemoryPlacement;
using engine::graphics::vulkan::resources::ResourceAllocator;

struct RunOptions final {
    bool bounded = false;
};

struct Vec3 final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Mat4 final {
    // Column-major, matching GLSL mat4.
    std::array<float, 16> m{};
};

struct CubePush final {
    Mat4 mvp{};
    float color_r = 0.0F;
    float color_g = 0.0F;
    float color_b = 0.0F;
    float color_a = 1.0F;
};

static_assert(sizeof(CubePush) == 80U);

struct CubeInstance final {
    Vec3 translation{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
    float color_r = 1.0F;
    float color_g = 1.0F;
    float color_b = 1.0F;
};

class ShutdownIdleGuard final {
  public:
    explicit ShutdownIdleGuard(VkDevice device) : device_(device) {}
    ~ShutdownIdleGuard() {
        if (device_ != VK_NULL_HANDLE) {
            static_cast<void>(vkDeviceWaitIdle(device_));
        }
    }

    ShutdownIdleGuard(const ShutdownIdleGuard &) = delete;
    ShutdownIdleGuard &operator=(const ShutdownIdleGuard &) = delete;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
};

RunOptions parseOptions(int argc, char **argv) {
    RunOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--bounded") {
            options.bounded = true;
            continue;
        }
        throw std::invalid_argument("Unknown application argument: " + std::string(argument));
    }
    return options;
}

std::vector<std::uint32_t> loadSpirv(const char *path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        throw std::runtime_error(std::string("Could not open generated SPIR-V: ") + path);
    }
    const std::streamsize byte_size = file.tellg();
    if (byte_size <= 0 || (byte_size % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
        throw std::runtime_error(std::string("Generated SPIR-V has invalid byte size: ") + path);
    }
    file.seekg(0);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(byte_size) / sizeof(std::uint32_t));
    if (!file.read(reinterpret_cast<char *>(words.data()), byte_size)) {
        throw std::runtime_error(std::string("Could not read generated SPIR-V: ") + path);
    }
    return words;
}

PixelExtent toPixelExtent(engine::platform::FramebufferExtent extent) {
    if (extent.width <= 0 || extent.height <= 0) {
        return {};
    }
    return {.width = static_cast<std::uint32_t>(extent.width),
            .height = static_cast<std::uint32_t>(extent.height)};
}

bool extentDiffers(const SwapchainGeneration &generation, PixelExtent extent) {
    return generation.config.extent.width != extent.width ||
           generation.config.extent.height != extent.height;
}

bool isSuccessfulPresent(engine::graphics::vulkan::presentation::PresentStatus status) {
    return status == engine::graphics::vulkan::presentation::PresentStatus::success ||
           status == engine::graphics::vulkan::presentation::PresentStatus::suboptimal;
}

GraphicsState makeGraphicsState(RenderingContext &rendering, std::span<const std::uint32_t> vertex,
                                std::span<const std::uint32_t> fragment, VkFormat color_format,
                                VkFormat depth_format) {
    constexpr std::array push_ranges{
        PushConstantRange{.stages = VK_SHADER_STAGE_VERTEX_BIT,
                          .offset = 0,
                          .size = static_cast<std::uint32_t>(sizeof(CubePush))},
    };
    return rendering.createGraphicsState({
        .vertex_spirv = vertex,
        .fragment_spirv = fragment,
        .push_constant_ranges = push_ranges,
        .color_format = color_format,
        .depth_format = depth_format,
        .depth_test_enable = true,
        .depth_write_enable = true,
        .depth_compare_op = VK_COMPARE_OP_LESS,
    });
}

Image createDepthImage(ResourceAllocator &allocator, VkFormat depth_format, PixelExtent extent) {
    ImageCreateInfo info;
    info.format = depth_format;
    info.extent = {.width = extent.width, .height = extent.height, .depth = 1};
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.allocation.placement = MemoryPlacement::prefer_device;
    return allocator.createImage(info);
}

void requireShutdownIdle(VkDevice device) {
    const VkResult result = vkDeviceWaitIdle(device);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("vkDeviceWaitIdle(shutdown) failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

Mat4 identity() {
    Mat4 out{};
    out.m[0] = 1.0F;
    out.m[5] = 1.0F;
    out.m[10] = 1.0F;
    out.m[15] = 1.0F;
    return out;
}

Mat4 mul(const Mat4 &a, const Mat4 &b) {
    Mat4 out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[static_cast<std::size_t>(k * 4 + row)] *
                       b.m[static_cast<std::size_t>(column * 4 + k)];
            }
            out.m[static_cast<std::size_t>(column * 4 + row)] = sum;
        }
    }
    return out;
}

Mat4 translation(Vec3 t) {
    Mat4 out = identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

Mat4 scale(Vec3 s) {
    Mat4 out = identity();
    out.m[0] = s.x;
    out.m[5] = s.y;
    out.m[10] = s.z;
    return out;
}

Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f{
        .x = center.x - eye.x,
        .y = center.y - eye.y,
        .z = center.z - eye.z,
    };
    const float f_len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    const Vec3 forward{.x = f.x / f_len, .y = f.y / f_len, .z = f.z / f_len};

    const Vec3 s{
        .x = forward.y * up.z - forward.z * up.y,
        .y = forward.z * up.x - forward.x * up.z,
        .z = forward.x * up.y - forward.y * up.x,
    };
    const float s_len = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    const Vec3 side{.x = s.x / s_len, .y = s.y / s_len, .z = s.z / s_len};
    const Vec3 up_n{
        .x = side.y * forward.z - side.z * forward.y,
        .y = side.z * forward.x - side.x * forward.z,
        .z = side.x * forward.y - side.y * forward.x,
    };

    Mat4 out = identity();
    out.m[0] = side.x;
    out.m[1] = up_n.x;
    out.m[2] = -forward.x;
    out.m[4] = side.y;
    out.m[5] = up_n.y;
    out.m[6] = -forward.y;
    out.m[8] = side.z;
    out.m[9] = up_n.z;
    out.m[10] = -forward.z;
    out.m[12] = -(side.x * eye.x + side.y * eye.y + side.z * eye.z);
    out.m[13] = -(up_n.x * eye.x + up_n.y * eye.y + up_n.z * eye.z);
    out.m[14] = forward.x * eye.x + forward.y * eye.y + forward.z * eye.z;
    return out;
}

Mat4 perspective(float fovy_radians, float aspect, float near_z, float far_z) {
    const float f = 1.0F / std::tan(fovy_radians * 0.5F);
    Mat4 out{};
    out.m[0] = f / aspect;
    out.m[5] = -f; // Vulkan NDC y is downward.
    out.m[10] = far_z / (near_z - far_z);
    out.m[11] = -1.0F;
    out.m[14] = (far_z * near_z) / (near_z - far_z);
    return out;
}

CubePush makeCubePush(const CubeInstance &cube, const Mat4 &view_proj) {
    const Mat4 model = mul(translation(cube.translation), scale(cube.scale));
    return CubePush{
        .mvp = mul(view_proj, model),
        .color_r = cube.color_r,
        .color_g = cube.color_g,
        .color_b = cube.color_b,
        .color_a = 1.0F,
    };
}

// Overlapping cubes at different view-space depths. Draw order is near→far so
// the probe is depth-adversarial: without depth testing/writing, later farther
// draws would incorrectly overwrite nearer geometry where they overlap. With a
// working public depth path, nearer geometry must still occlude farther geometry.
constexpr std::array kCubes{
    CubeInstance{{-0.35F, 0.05F, -1.6F}, {1.1F, 1.1F, 1.1F}, 0.92F, 0.28F, 0.28F}, // nearer, red
    CubeInstance{{0.0F, 0.55F, -2.4F}, {0.9F, 0.9F, 0.9F}, 0.28F, 0.78F, 0.42F},   // mid, green
    CubeInstance{{-0.8F, -0.55F, -2.8F}, {0.8F, 0.8F, 0.8F}, 0.95F, 0.78F, 0.22F}, // yellow
    CubeInstance{{0.35F, -0.05F, -3.2F}, {1.4F, 1.4F, 1.4F}, 0.28F, 0.52F, 0.95F}, // farther, blue
};

} // namespace

int main(int argc, char **argv) {
    try {
        const RunOptions options = parseOptions(argc, argv);
        const auto vertex_spirv = loadSpirv(GAME_3D_VERTEX_SPV);
        const auto fragment_spirv = loadSpirv(GAME_3D_FRAGMENT_SPV);

        engine::platform::WindowSystem window_system;
        const auto wsi_requirements =
            engine::graphics::vulkan::wsi::queryInstanceRequirements(window_system);
        const auto presentation_requirements =
            engine::graphics::vulkan::presentation::queryInstanceRequirements();
        const auto wsi_views = wsi_requirements.views();
        const auto presentation_views = presentation_requirements.views();
        std::vector<std::string_view> required_extensions;
        required_extensions.reserve(wsi_views.size() + presentation_views.size());
        required_extensions.insert(required_extensions.end(), wsi_views.begin(), wsi_views.end());
        required_extensions.insert(required_extensions.end(), presentation_views.begin(),
                                   presentation_views.end());
        const engine::graphics::vulkan::Runtime runtime{
            {.required_instance_extensions =
                 std::span<const std::string_view>{required_extensions}}};

        engine::platform::WindowConfig config;
        config.title = "game-3d";
        config.visible = !options.bounded;
        const engine::platform::Window window{window_system, config};
        const engine::graphics::vulkan::wsi::Surface surface{runtime.instance(), window};
        const auto selected = engine::graphics::vulkan::device::selectPhysicalDevice(
            runtime.instance(), surface.handle(), runtime.applicationApiVersion());
        const engine::graphics::vulkan::logical_device::LogicalDevice logical_device{selected};
        ResourceAllocator allocator{runtime.instance(), selected, logical_device};
        engine::graphics::vulkan::execution::GraphicsExecutionContext execution{
            logical_device.handle(), logical_device.queues().graphics};
        engine::graphics::vulkan::presentation::Swapchain swapchain{
            selected.capabilities.handle, logical_device.handle(), surface.handle(),
            logical_device.queues()};
        RenderingContext rendering{selected, logical_device, allocator, execution};

        const VkFormat depth_format = rendering.selectDepthAttachmentFormat();
        if (!rendering.supportsDepthAttachmentFormat(depth_format)) {
            throw std::runtime_error("Selected Rendering depth format failed support query.");
        }

        std::optional<GraphicsState> graphics_state;
        std::optional<Image> depth_image;
        std::optional<ResourceDepthTarget> depth_target;
        float aspect = 1.0F;

        bool recreate_requested = true;
        std::uint32_t rendered_frames = 0;
        std::uint32_t bounded_iterations = 0;
        constexpr std::uint32_t bounded_frame_target = 4;
        constexpr std::uint32_t bounded_iteration_limit = 512;

        const auto recreate = [&](PixelExtent framebuffer_extent) {
            const std::optional<std::uint64_t> previous_generation =
                swapchain.hasActiveGeneration()
                    ? std::optional<std::uint64_t>{swapchain.activeGeneration().id}
                    : std::nullopt;
            const auto outcome = swapchain.recreate(framebuffer_extent);
            if (previous_generation.has_value()) {
                const bool still_active = swapchain.hasActiveGeneration() &&
                                          swapchain.activeGeneration().id == *previous_generation;
                if (!still_active) {
                    rendering.retireSwapchainGeneration(*previous_generation);
                    std::cout << "Swapchain generation retired: " << *previous_generation << '\n';
                }
            }
            if (outcome.status ==
                engine::graphics::vulkan::presentation::RecreateStatus::surface_lost) {
                throw std::runtime_error("Presentation surface was lost during recreation.");
            }
            if (outcome.status == engine::graphics::vulkan::presentation::RecreateStatus::created) {
                rendering.registerSwapchainGeneration(swapchain.activeGeneration());
                rendering.waitForSubmittedWork();
                depth_target.reset();
                depth_image.reset();
                const auto &active = swapchain.activeGeneration();
                const PixelExtent depth_extent{.width = active.config.extent.width,
                                               .height = active.config.extent.height};
                depth_image = createDepthImage(allocator, depth_format, depth_extent);
                depth_target = rendering.createDepthTarget({.image = &*depth_image});
                graphics_state =
                    makeGraphicsState(rendering, vertex_spirv, fragment_spirv,
                                      active.config.surface_format.format, depth_format);
                aspect = static_cast<float>(active.config.extent.width) /
                         static_cast<float>(active.config.extent.height);
                std::cout << "Swapchain generation active: " << active.id
                          << " extent=" << active.config.extent.width << 'x'
                          << active.config.extent.height << " aspect=" << aspect
                          << " depth_format=" << depth_format
                          << " images=" << active.images.size() << '\n';
            }
            return outcome;
        };

        const PixelExtent initial_extent = toPixelExtent(window.framebufferExtent());
        if (initial_extent.width != 0U && initial_extent.height != 0U) {
            const auto outcome = recreate(initial_extent);
            recreate_requested =
                outcome.status != engine::graphics::vulkan::presentation::RecreateStatus::created;
        }

        const ShutdownIdleGuard shutdown_idle{logical_device.handle()};
        static_cast<void>(shutdown_idle);

        std::cout << "game-3d initialized: " << selected.capabilities.name << " (API "
                  << VK_API_VERSION_MAJOR(selected.capabilities.effective_api_version) << '.'
                  << VK_API_VERSION_MINOR(selected.capabilities.effective_api_version)
                  << ") validation=" << (runtime.validationEnabled() ? "enabled" : "disabled")
                  << '\n';
        std::cout << "cubes=" << kCubes.size() << " depth_format=" << depth_format << '\n';
        std::cout << "hidden_surface_probe=draw_order_near_to_far_adversarial "
                     "(without depth farther overwrites; with depth nearer still wins)\n";

        const Mat4 view = lookAt({0.0F, 0.35F, 1.8F}, {0.0F, 0.0F, -2.2F}, {0.0F, 1.0F, 0.0F});

        while (!window.shouldClose()) {
            if (options.bounded && rendered_frames >= bounded_frame_target) {
                break;
            }
            if (options.bounded && ++bounded_iterations > bounded_iteration_limit) {
                throw std::runtime_error("Bounded rendering did not complete its presentation "
                                         "target deterministically.");
            }

            window_system.pollEvents();
            const PixelExtent framebuffer_extent = toPixelExtent(window.framebufferExtent());
            if (framebuffer_extent.width == 0U || framebuffer_extent.height == 0U) {
                std::this_thread::sleep_for(std::chrono::milliseconds{8});
                continue;
            }

            if (swapchain.hasActiveGeneration() &&
                extentDiffers(swapchain.activeGeneration(), framebuffer_extent)) {
                recreate_requested = true;
            }
            if (recreate_requested || !swapchain.hasActiveGeneration()) {
                const auto outcome = recreate(framebuffer_extent);
                if (outcome.status ==
                    engine::graphics::vulkan::presentation::RecreateStatus::deferred) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{8});
                    continue;
                }
                recreate_requested = false;
            }

            for (const std::uint64_t generation :
                 rendering.collectRetiredSwapchainGenerations(swapchain)) {
                std::cout << "Retired swapchain generation destroyed: " << generation << '\n';
            }

            const auto acquired = rendering.acquireNextImage(swapchain);
            if (acquired.status ==
                    engine::graphics::vulkan::presentation::AcquireStatus::not_ready ||
                acquired.status == engine::graphics::vulkan::presentation::AcquireStatus::timeout) {
                continue;
            }
            if (acquired.status ==
                engine::graphics::vulkan::presentation::AcquireStatus::out_of_date) {
                recreate_requested = true;
                continue;
            }
            if (acquired.status ==
                engine::graphics::vulkan::presentation::AcquireStatus::surface_lost) {
                throw std::runtime_error("Presentation surface was lost during image acquisition.");
            }
            if (!acquired.image.has_value() || !graphics_state.has_value() ||
                !depth_target.has_value()) {
                throw std::logic_error("Rendering acquire succeeded without frame state.");
            }
            if (acquired.status ==
                engine::graphics::vulkan::presentation::AcquireStatus::suboptimal) {
                recreate_requested = true;
            }

            const Mat4 proj = perspective(0.785398163F, aspect, 0.1F, 32.0F);
            const Mat4 view_proj = mul(proj, view);

            std::array<CubePush, kCubes.size()> pushes{};
            std::array<PushConstantWrite, kCubes.size()> push_writes{};
            std::array<DrawInfo, kCubes.size()> draws{};
            for (std::size_t index = 0; index < kCubes.size(); ++index) {
                pushes[index] = makeCubePush(kCubes[index], view_proj);
                push_writes[index] = PushConstantWrite{
                    .stages = VK_SHADER_STAGE_VERTEX_BIT,
                    .offset = 0,
                    .data = std::as_bytes(std::span{&pushes[index], 1}),
                };
                draws[index] = DrawInfo{
                    .graphics_state = &*graphics_state,
                    .push_constants = std::span<const PushConstantWrite>{&push_writes[index], 1},
                    .vertex_count = 36,
                    .instance_count = 1,
                };
            }

            VkClearColorValue clear{};
            clear.float32[0] = 0.04F;
            clear.float32[1] = 0.05F;
            clear.float32[2] = 0.09F;
            clear.float32[3] = 1.0F;
            const ColorPass pass{
                .draws = draws,
                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .store_op = VK_ATTACHMENT_STORE_OP_STORE,
                .clear_color = clear,
                .depth_target = &*depth_target,
                .depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clear_depth = 1.0F,
            };

            const auto frame = rendering.renderAndPresent(swapchain, *acquired.image, pass);
            if (frame.present.status ==
                engine::graphics::vulkan::presentation::PresentStatus::surface_lost) {
                throw std::runtime_error(
                    "Presentation surface was lost during queue presentation.");
            }
            if (frame.present.status ==
                    engine::graphics::vulkan::presentation::PresentStatus::suboptimal ||
                frame.present.status ==
                    engine::graphics::vulkan::presentation::PresentStatus::out_of_date) {
                recreate_requested = true;
            }
            if (!isSuccessfulPresent(frame.present.status)) {
                continue;
            }
            ++rendered_frames;
            if (options.bounded) {
                std::cout << "frame=" << rendered_frames
                          << " generation=" << acquired.image->generation
                          << " image=" << acquired.image->image_index
                          << " completion=" << frame.completion.value << '\n';
            }
        }

        rendering.waitForSubmittedWork();
        for (const std::uint64_t generation :
             rendering.collectRetiredSwapchainGenerations(swapchain)) {
            std::cout << "Retired swapchain generation destroyed: " << generation << '\n';
        }
        depth_target.reset();
        depth_image.reset();
        requireShutdownIdle(logical_device.handle());

        std::cout << "game_3d=passed frames=" << rendered_frames << " cubes=" << kCubes.size()
                  << (options.bounded ? " mode=bounded\n" : " mode=interactive\n");
        std::cout << "occlusion=depth_tested_nearer_wins\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "game_3d=failed: " << error.what() << '\n';
        return 1;
    }
}
