#include <engine/graphics/vulkan/graphics.hpp>
#include <engine/platform/window.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
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

using engine::graphics::vulkan::FrameBeginDisposition;
using engine::graphics::vulkan::FrameBeginResult;
using engine::graphics::vulkan::FrameEndDisposition;
using engine::graphics::vulkan::GraphicsContext;
using engine::graphics::vulkan::presentation::PixelExtent;
using engine::graphics::vulkan::rendering::ColorPass;
using engine::graphics::vulkan::rendering::DrawInfo;
using engine::graphics::vulkan::rendering::GraphicsState;
using engine::graphics::vulkan::rendering::PushConstantRange;
using engine::graphics::vulkan::rendering::PushConstantWrite;
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

GraphicsState makeGraphicsState(const GraphicsContext &graphics,
                                std::span<const std::uint32_t> vertex,
                                std::span<const std::uint32_t> fragment, VkFormat color_format,
                                VkFormat depth_format) {
    constexpr std::array push_ranges{
        PushConstantRange{.stages = VK_SHADER_STAGE_VERTEX_BIT,
                          .offset = 0,
                          .size = static_cast<std::uint32_t>(sizeof(CubePush))},
    };
    return graphics.createGraphicsState({
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
        engine::platform::WindowConfig config;
        config.title = "game-3d";
        config.visible = !options.bounded;
        const engine::platform::Window window{window_system, config};
        GraphicsContext graphics{window_system, window};

        const VkFormat depth_format = graphics.selectDepthAttachmentFormat();
        if (!graphics.supportsDepthAttachmentFormat(depth_format)) {
            throw std::runtime_error("Selected Rendering depth format failed support query.");
        }

        std::optional<GraphicsState> graphics_state;
        std::optional<VkFormat> graphics_state_color_format;
        std::optional<Image> depth_image;
        std::optional<ResourceDepthTarget> depth_target;
        std::optional<PixelExtent> depth_extent;
        float aspect = 1.0F;

        std::uint32_t rendered_frames = 0;
        std::uint32_t bounded_iterations = 0;
        constexpr std::uint32_t bounded_frame_target = 4;
        constexpr std::uint32_t bounded_iteration_limit = 512;

        const auto applyReadyFrame = [&](const FrameBeginResult &frame) {
            if (frame.disposition != FrameBeginDisposition::ready) {
                throw std::logic_error("Frame state requires a ready begin result.");
            }
            if (frame.extent.width == 0U || frame.extent.height == 0U ||
                frame.color_format == VK_FORMAT_UNDEFINED) {
                throw std::logic_error("Ready frame reported invalid presentation state.");
            }

            const bool replace_graphics_state =
                !graphics_state.has_value() || !graphics_state_color_format.has_value() ||
                *graphics_state_color_format != frame.color_format;
            const bool replace_depth =
                !depth_image.has_value() || !depth_target.has_value() || !depth_extent.has_value() ||
                depth_extent->width != frame.extent.width || depth_extent->height != frame.extent.height;

            const bool existing_graphics_replaced = replace_graphics_state && graphics_state.has_value();
            const bool existing_depth_replaced =
                replace_depth && (depth_target.has_value() || depth_image.has_value());
            if (existing_graphics_replaced || existing_depth_replaced) {
                graphics.waitForSubmittedWork();
            }

            if (replace_graphics_state) {
                graphics_state.reset();
                graphics_state = makeGraphicsState(graphics, vertex_spirv, fragment_spirv,
                                                   frame.color_format, depth_format);
                graphics_state_color_format = frame.color_format;
            }

            if (replace_depth) {
                depth_target.reset();
                depth_image.reset();
                depth_image = createDepthImage(graphics.resources(), depth_format, frame.extent);
                depth_target = graphics.createDepthTarget({.image = &*depth_image});
                depth_extent = frame.extent;
            }

            aspect = static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);
            if (frame.generation_changed) {
                std::cout << "Swapchain generation active: " << frame.generation
                          << " extent=" << frame.extent.width << 'x' << frame.extent.height
                          << " aspect=" << aspect << " depth_format=" << depth_format << '\n';
            }
        };

        const std::uint32_t api_version = graphics.applicationApiVersion();
        std::cout << "game-3d initialized: " << graphics.deviceName() << " (API "
                  << VK_API_VERSION_MAJOR(api_version) << '.' << VK_API_VERSION_MINOR(api_version)
                  << ")\n";
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
            const auto frame = graphics.beginFrame(window.framebufferExtent());
            for (const std::uint64_t generation : frame.reclaimed_generations) {
                std::cout << "Retired swapchain generation destroyed: " << generation << '\n';
            }

            switch (frame.disposition) {
            case FrameBeginDisposition::deferred:
                std::this_thread::sleep_for(std::chrono::milliseconds{8});
                continue;
            case FrameBeginDisposition::try_again:
                continue;
            case FrameBeginDisposition::surface_lost:
                throw std::runtime_error("Presentation surface was lost while beginning a frame.");
            case FrameBeginDisposition::ready:
                break;
            }

            applyReadyFrame(frame);
            if (!graphics_state.has_value() || !depth_target.has_value()) {
                throw std::logic_error("Ready frame has no compatible depth-capable frame state.");
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

            const auto presented = graphics.presentFrame(pass);
            switch (presented.disposition) {
            case FrameEndDisposition::surface_lost:
                throw std::runtime_error("Presentation surface was lost during queue presentation.");
            case FrameEndDisposition::try_again:
                continue;
            case FrameEndDisposition::accepted:
                break;
            }

            ++rendered_frames;
            if (options.bounded) {
                std::cout << "frame=" << rendered_frames << " generation=" << frame.generation
                          << " image=" << frame.image_index
                          << " completion=" << presented.completion.value << '\n';
            }
        }

        graphics.waitForSubmittedWork();
        graphics_state.reset();
        depth_target.reset();
        depth_image.reset();

        std::cout << "game_3d=passed frames=" << rendered_frames << " cubes=" << kCubes.size()
                  << (options.bounded ? " mode=bounded\n" : " mode=interactive\n");
        std::cout << "occlusion=depth_tested_nearer_wins\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "game_3d=failed: " << error.what() << '\n';
        return 1;
    }
}
