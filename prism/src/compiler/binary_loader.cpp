#include "prism/compiler/binary_loader.hpp"
#include "prism/compiler/binary_format.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/modifiers/blur_modifier.hpp"
#include "prism/modifiers/geometry_modifier.hpp"
#include "prism/modifiers/animation_modifier.hpp"
#include "prism/modifiers/acrylic_modifier.hpp"
#include "prism/modifiers/glow_modifier.hpp"
#include "prism/modifiers/spring_hover_modifier.hpp"
#include "prism/core/logging.hpp"
#include "prism/core/types.hpp"
#include "prism/pack/package.hpp"
#include "prism/decoration/tiling_decoration_spec.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace prism::compiler {

std::shared_ptr<scene::SceneNode> BinarySceneLoader::LoadFromFile(const std::string& prismb_path) {
    if (size_t col = prismb_path.find(".prismpkg:"); col != std::string::npos) {
        std::string pkg = prismb_path.substr(0, col + 9);
        std::string internal_name = prismb_path.substr(col + 10);
        return LoadFromPackage(pkg, internal_name);
    }

    int fd = open(prismb_path.c_str(), O_RDONLY);
    if (fd < 0) {
        PRISM_LOG_ERROR("LOADER", "Failed to open binary file: %s", prismb_path.c_str());
        return nullptr;
    }

    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(PrismbHeader))) {
        PRISM_LOG_ERROR("LOADER", "Invalid file size for: %s", prismb_path.c_str());
        close(fd);
        return nullptr;
    }

    void* mapped = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapped == MAP_FAILED) {
        PRISM_LOG_ERROR("LOADER", "mmap failed for: %s", prismb_path.c_str());
        return nullptr;
    }

    auto t0 = core::CurrentTimeNs();
    auto root = LoadFromMemory(static_cast<const uint8_t*>(mapped), st.st_size);
    auto elapsed_us = (core::CurrentTimeNs() - t0) / 1000.0;

    munmap(mapped, st.st_size);

    PRISM_LOG_INFO("LOADER", "AOT binary '%s' loaded via zero-copy mmap in %.2f us (%.4f ms)",
                   prismb_path.c_str(), elapsed_us, elapsed_us / 1000.0);
    return root;
}

std::shared_ptr<scene::SceneNode> BinarySceneLoader::LoadFromPackage(const std::string& pkg_path, const std::string& internal_file_name) {
    auto data = pack::PackageManager::ExtractFile(pkg_path, internal_file_name);
    if (data.empty()) {
        PRISM_LOG_ERROR("LOADER", "Binary '%s' not found in package '%s'", internal_file_name.c_str(), pkg_path.c_str());
        return nullptr;
    }
    auto t0 = core::CurrentTimeNs();
    auto root = LoadFromMemory(data.data(), data.size());
    auto elapsed_us = (core::CurrentTimeNs() - t0) / 1000.0;
    PRISM_LOG_INFO("LOADER", "AOT binary '%s' loaded from package '%s' in %.2f us (%.4f ms)",
                   internal_file_name.c_str(), pkg_path.c_str(), elapsed_us, elapsed_us / 1000.0);
    return root;
}

std::shared_ptr<scene::SceneNode> BinarySceneLoader::LoadFromMemory(const uint8_t* data, size_t size) {
    if (size < sizeof(PrismbHeader)) return nullptr;

    const auto* header = reinterpret_cast<const PrismbHeader*>(data);
    if (header->magic != PRISMB_MAGIC || header->version != PRISMB_VERSION) {
        PRISM_LOG_ERROR("LOADER", "Prism binary magic/version mismatch! Expected v%d, got v%d",
                        PRISMB_VERSION, header->version);
        return nullptr;
    }

    const auto* records = reinterpret_cast<const PrismbNodeRecord*>(data + sizeof(PrismbHeader));
    const char* str_table = reinterpret_cast<const char*>(data + header->string_table_offset);

    auto GetString = [&](uint32_t offset) -> std::string {
        if (offset == 0xFFFFFFFF || offset >= header->string_table_size) return "";
        return std::string(str_table + offset);
    };

    std::vector<std::shared_ptr<scene::SceneNode>> nodes(header->node_count);

    // 1. Instantiate Nodes and Modifiers
    for (size_t i = 0; i < header->node_count; ++i) {
        const auto& rec = records[i];
        std::string name = GetString(rec.name_offset);
        std::shared_ptr<scene::SceneNode> node;

        switch (rec.node_type) {
            case BinaryNodeType::VStack:
                node = std::make_shared<scene::VStackNode>(rec.spacing, name.empty() ? "VStack" : name);
                break;
            case BinaryNodeType::HStack:
                node = std::make_shared<scene::HStackNode>(rec.spacing, name.empty() ? "HStack" : name);
                break;
            case BinaryNodeType::Text: {
                auto tn = std::make_shared<scene::TextNode>(GetString(rec.text_offset), name.empty() ? "Text" : name);
                if (rec.height > 0) tn->SetFontSize(rec.height);
                node = tn;
                break;
            }
            case BinaryNodeType::Button:
                node = std::make_shared<scene::ButtonNode>(GetString(rec.text_offset), GetString(rec.action_offset), name.empty() ? "Button" : name);
                break;
            case BinaryNodeType::Slider:
                node = std::make_shared<scene::SliderNode>(0.0, name.empty() ? "Slider" : name);
                break;
            case BinaryNodeType::Skeleton:
                node = std::make_shared<scene::SkeletonNode>(GetString(rec.text_offset), rec.height > 0 ? rec.height : 1.0f, name.empty() ? "Skeleton" : name);
                break;
            case BinaryNodeType::Icon:
                node = std::make_shared<scene::IconNode>(GetString(rec.icon_offset), rec.width > 0 ? rec.width : 1.0f, name.empty() ? "Icon" : name);
                break;
            case BinaryNodeType::Toggle:
                node = std::make_shared<scene::ToggleNode>(rec.width > 0.5f, name.empty() ? "Toggle" : name);
                break;
            case BinaryNodeType::TextInput:
                node = std::make_shared<scene::TextInputNode>("", GetString(rec.text_offset), "", name.empty() ? "TextInput" : name);
                break;
            case BinaryNodeType::ProgressBar:
                node = std::make_shared<scene::ProgressBarNode>(rec.width, name.empty() ? "ProgressBar" : name);
                break;
            case BinaryNodeType::Card:
                node = std::make_shared<scene::CardNode>(rec.spacing > 0 ? rec.spacing : 12.0f, name.empty() ? "Card" : name);
                break;
            case BinaryNodeType::Spacer:
                node = std::make_shared<scene::SpacerNode>(rec.width, name.empty() ? "Spacer" : name);
                break;
            case BinaryNodeType::Badge:
                node = std::make_shared<scene::BadgeNode>(GetString(rec.text_offset), name.empty() ? "Badge" : name);
                break;
            case BinaryNodeType::ZStack:
                node = std::make_shared<scene::ZStackNode>(name.empty() ? "ZStack" : name);
                break;
            case BinaryNodeType::Desktop:
                node = std::make_shared<scene::DesktopNode>(name.empty() ? "Desktop" : name);
                break;
            case BinaryNodeType::TopBar:
                node = std::make_shared<scene::TopBarNode>(rec.spacing > 0 ? rec.spacing : 8.0f, name.empty() ? "TopBar" : name);
                break;
            case BinaryNodeType::Dock:
                node = std::make_shared<scene::DockNode>(rec.spacing > 0 ? rec.spacing : 10.0f, name.empty() ? "Dock" : name);
                break;
            case BinaryNodeType::AppGroup:
                node = std::make_shared<scene::AppGroupNode>(name.empty() ? "AppGroup" : name);
                break;
            default:
                node = std::make_shared<scene::VStackNode>(0.0f, "Unknown");
                break;
        }

        // Slot ID
        if (rec.slot_id != 0) {
            node->SetSlot(rec.slot_id);
        }

        // Attach Modifiers (Decorator Pattern)
        if (rec.blur_radius > 0) {
            if (rec.tint_color != 0) {
                node->Modifiers().Add(std::make_shared<modifiers::AcrylicModifier>(
                    rec.blur_radius, rec.blur_passes > 0 ? rec.blur_passes : 4, core::Color::FromHex(rec.tint_color)));
            } else {
                node->Modifiers().Add(std::make_shared<modifiers::KawaseBlurModifier>(
                    rec.blur_radius, rec.blur_passes > 0 ? rec.blur_passes : 4));
            }
        }
        if (rec.glow_radius > 0 || rec.glow_color != 0) {
            node->Modifiers().Add(std::make_shared<modifiers::GlowModifier>(
                rec.glow_radius, core::Color::FromHex(rec.glow_color)));
        }
        if (rec.hover_scale > 0) {
            node->Modifiers().Add(std::make_shared<modifiers::HoverSpringModifier>(
                rec.hover_scale, rec.spring_damping > 0 ? rec.spring_damping : 0.8f));
        }
        if (rec.corner_radius > 0) {
            node->Modifiers().Add(std::make_shared<modifiers::CornerRadiusModifier>(rec.corner_radius));
        }
        if (rec.padding > 0) {
            node->Modifiers().Add(std::make_shared<modifiers::PaddingModifier>(rec.padding));
        }
        if (rec.spring_damping > 0 && rec.hover_scale <= 0) {
            node->Modifiers().Add(std::make_shared<modifiers::SpringAnimationModifier>(rec.spring_damping, rec.spring_stiffness));
        }

        nodes[i] = node;
    }

    // 2. Re-establish Composite Hierarchy via LCRS (Left-Child Right-Sibling)
    for (size_t i = 0; i < header->node_count; ++i) {
        const auto& rec = records[i];
        if (rec.first_child_index != 0xFFFF && rec.first_child_index < header->node_count) {
            if (auto container = std::dynamic_pointer_cast<scene::ContainerNode>(nodes[i])) {
                uint16_t child_idx = rec.first_child_index;
                while (child_idx != 0xFFFF && child_idx < header->node_count) {
                    container->AddChild(nodes[child_idx]);
                    child_idx = records[child_idx].next_sibling_index;
                }
            }
        }
    }

    return nodes.empty() ? nullptr : nodes[0];
}

std::shared_ptr<decoration::TilingDecorationSpec> BinaryThemeLoader::LoadFromFile(const std::string& prismb_path) {
    int fd = open(prismb_path.c_str(), O_RDONLY);
    if (fd < 0) {
        PRISM_LOG_ERROR("LOADER", "Failed to open theme file: %s", prismb_path.c_str());
        return nullptr;
    }

    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(PrismbThemeHeader))) {
        PRISM_LOG_ERROR("LOADER", "Invalid file size for theme: %s", prismb_path.c_str());
        close(fd);
        return nullptr;
    }

    void* mapped = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapped == MAP_FAILED) {
        PRISM_LOG_ERROR("LOADER", "mmap failed for theme: %s", prismb_path.c_str());
        return nullptr;
    }

    auto t0 = core::CurrentTimeNs();
    auto theme = LoadFromMemory(static_cast<const uint8_t*>(mapped), st.st_size);
    auto elapsed_us = (core::CurrentTimeNs() - t0) / 1000.0;

    munmap(mapped, st.st_size);

    if (theme) {
        PRISM_LOG_INFO("LOADER", "AOT theme '%s' ('%s') loaded via zero-copy mmap in %.2f us (%.4f ms)",
                       prismb_path.c_str(), theme->theme_name.c_str(), elapsed_us, elapsed_us / 1000.0);
    }
    return theme;
}

std::shared_ptr<decoration::TilingDecorationSpec> BinaryThemeLoader::LoadFromMemory(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(PrismbThemeHeader)) return nullptr;
    const auto* header = reinterpret_cast<const PrismbThemeHeader*>(data);
    if (header->magic != PRISMB_THEME_MAGIC) {
        PRISM_LOG_ERROR("LOADER", "Theme magic mismatch: expected 0x%08X (THEM), got 0x%08X", PRISMB_THEME_MAGIC, header->magic);
        return nullptr;
    }

    auto spec = std::make_shared<decoration::TilingDecorationSpec>();
    spec->theme_name = header->theme_name;
    spec->gaps.inner = header->inner_gap;
    spec->gaps.outer = header->outer_gap;
    spec->gaps.smart_gaps = (header->smart_gaps != 0);

    spec->border.width = header->border_width;
    spec->border.color_focused = core::Color::FromHex(header->border_color_focused);
    spec->border.color_unfocused = core::Color::FromHex(header->border_color_unfocused);
    spec->border.top_rim_specular = core::Color::FromHex(header->top_rim_specular);
    spec->border.corner_radius = header->corner_radius;

    spec->backdrop.bg_focused = core::Color::FromHex(header->bg_focused);
    spec->backdrop.bg_unfocused = core::Color::FromHex(header->bg_unfocused);
    spec->backdrop.blur_radius = header->blur_radius;
    spec->backdrop.blur_passes = header->blur_passes;

    spec->header.height = header->header_height;
    spec->header.show_header = (header->show_header != 0);
    spec->header.bg_focused = core::Color::FromHex(header->header_bg_focused);
    spec->header.bg_unfocused = core::Color::FromHex(header->header_bg_unfocused);
    spec->header.title_focused = core::Color::FromHex(header->title_color_focused);
    spec->header.title_unfocused = core::Color::FromHex(header->title_color_unfocused);
    spec->header.show_tiling_controls = (header->show_tiling_controls != 0);

    spec->drop_zone.fill_color = core::Color::FromHex(header->drop_fill_color);
    spec->drop_zone.border_color = core::Color::FromHex(header->drop_border_color);
    spec->drop_zone.border_width = header->drop_border_width;

    // Load Kinetic Motion records
    auto load_motion = [](const PrismbMotionCurveRecord& rec, decoration::MotionCurveSpec& out) {
        out.engine = static_cast<decoration::MotionEngine>(rec.engine_type);
        out.duration_ms = rec.duration_ms;
        out.clip_content = (rec.flags & 0x01) != 0;
        out.fade_content = (rec.flags & 0x02) != 0;
        out.smart_gaps_collapse = (rec.flags & 0x04) != 0;

        if (out.engine == decoration::MotionEngine::Spring) {
            out.damping = rec.param0;
            out.stiffness = rec.param1;
        } else {
            out.bezier_x1 = rec.param0;
            out.bezier_y1 = rec.param1;
            out.bezier_x2 = rec.param2;
            out.bezier_y2 = rec.param3;
        }
    };

    load_motion(header->motion_fold, spec->motion.fold);
    load_motion(header->motion_fullscreen, spec->motion.fullscreen);
    load_motion(header->motion_split_move, spec->motion.split_move);
    load_motion(header->motion_focus, spec->motion.focus);

    return spec;
}

} // namespace prism::compiler
