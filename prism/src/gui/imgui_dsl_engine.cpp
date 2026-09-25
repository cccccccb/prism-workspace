#include "prism/gui/imgui_dsl_engine.hpp"
#include "prism/core/logging.hpp"
#include "imgui.h"
#include "imgui_sw.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace prism::gui {

ImGuiDslEngine::ImGuiDslEngine() = default;

ImGuiDslEngine::~ImGuiDslEngine() {
    Shutdown();
}

bool ImGuiDslEngine::Initialize(int width, int height) {
    if (initialized_) return true;

    width_ = width;
    height_ = height;

    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    if (!ctx_) {
        PRISM_LOG_ERROR("IMGUI-DSL", "Failed to create Dear ImGui context");
        return false;
    }

    ImGui::SetCurrentContext(ctx_);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Stateless - no .ini file
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));

    // Try loading system vector fonts for crisp anti-aliased rendering
    std::vector<std::string> font_candidates = {
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-M.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    };

    bool font_loaded = false;
    for (const auto& fp : font_candidates) {
        if (std::filesystem::exists(fp)) {
            if (LoadFont(fp, 15.0f)) {
                font_loaded = true;
                break;
            }
        }
    }

    if (!font_loaded) {
        // Fallback to ImGui default proggy font
        imgui_sw::bind_imgui_painting();
    }

    // Configure macOS Dark Acrylic Frosted Styling
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 16.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);

    // Modern glass color scheme
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // 100% Transparent (backdrop handled by Prism)
    colors[ImGuiCol_Text]           = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Ultra-crisp pure white
    colors[ImGuiCol_TextDisabled]   = ImVec4(0.6f, 0.65f, 0.72f, 0.8f);
    colors[ImGuiCol_Button]         = ImVec4(0.20f, 0.24f, 0.32f, 0.50f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.28f, 0.35f, 0.48f, 0.75f);
    colors[ImGuiCol_ButtonActive]   = ImVec4(0.00f, 0.48f, 1.00f, 0.90f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.12f, 0.16f, 0.22f, 0.60f);

    initialized_ = true;
    PRISM_LOG_INFO("IMGUI-DSL", "ImGuiDslEngine initialized (%dx%d, font_loaded=%d)",
                   width, height, font_loaded ? 1 : 0);
    return true;
}

void ImGuiDslEngine::Shutdown() {
    if (!initialized_) return;

    if (ctx_) {
        ImGui::SetCurrentContext(ctx_);
        imgui_sw::unbind_imgui_painting();
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
    }
    initialized_ = false;
    PRISM_LOG_INFO("IMGUI-DSL", "ImGuiDslEngine shutdown cleanly");
}

bool ImGuiDslEngine::LoadFont(const std::string& font_path, float font_size_px) {
    if (!std::filesystem::exists(font_path)) return false;

    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x2000, 0x206F, // General Punctuation
        0x25A0, 0x25FF, // Geometric Shapes (●, ■, ▲, etc.)
        0x2600, 0x26FF, // Miscellaneous Symbols (⚙, ★, ♫, etc.)
        0x4E00, 0x9FFF, // CJK Unified Ideographs (田, etc.)
        0,
    };

    ImFont* f = io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size_px, &cfg, ranges);
    if (!f) {
        PRISM_LOG_WARN("IMGUI-DSL", "Failed to rasterize TTF font '%s'", font_path.c_str());
        return false;
    }

    // Merge fallback fonts for symbols or CJK if present
    std::vector<std::string> fallback_candidates = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
    };
    for (const auto& fb_path : fallback_candidates) {
        if (fb_path != font_path && std::filesystem::exists(fb_path)) {
            ImFontConfig merge_cfg;
            merge_cfg.MergeMode = true;
            merge_cfg.OversampleH = 2;
            merge_cfg.OversampleV = 2;
            io.Fonts->AddFontFromFileTTF(fb_path.c_str(), font_size_px, &merge_cfg, ranges);
            break;
        }
    }

    imgui_sw::bind_imgui_painting();
    PRISM_LOG_INFO("IMGUI-DSL", "Loaded vector TTF font: '%s' (%.1fpx)", font_path.c_str(), font_size_px);
    return true;
}

void ImGuiDslEngine::SetState(const std::string& key, const std::string& value) {
    state_store_[key] = value;
}

std::string ImGuiDslEngine::GetState(const std::string& key, const std::string& default_val) const {
    auto it = state_store_.find(key);
    if (it != state_store_.end()) return it->second;
    return default_val;
}

void ImGuiDslEngine::UpdateMouse(float mouse_x, float mouse_y, bool mouse_down) {
    if (!ctx_) return;
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(mouse_x, mouse_y);
    io.MouseDown[0] = mouse_down;
}

std::string ImGuiDslEngine::ResolveValue(const std::string& fallback, const std::string& slot) const {
    if (!slot.empty()) {
        std::string key = slot;
        if (key.starts_with("$")) key = key.substr(1);
        auto it = state_store_.find(key);
        if (it != state_store_.end()) return it->second;
    }
    return fallback;
}

void ImGuiDslEngine::RenderTree(const std::shared_ptr<compiler::AstNode>& root,
                                render::FrameBuffer& target_fb,
                                float dt) {
    if (!initialized_ || !root || !ctx_) return;

    ImGui::SetCurrentContext(ctx_);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(target_fb.GetWidth()), static_cast<float>(target_fb.GetHeight()));
    io.DeltaTime = dt > 0.0001f ? dt : 0.016f;

    topbar_spacer_count_ = 0;
    dock_button_index_ = 0;

    ImGui::NewFrame();

    // Full viewport invisible canvas
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (ImGui::Begin("##PrismDslRoot", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings)) {
        RenderNode(root, dt);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();

    // Software rasterization directly onto FrameBuffer pixels
    imgui_sw::paint_imgui(target_fb.GetPixelsMutable(), target_fb.GetWidth(), target_fb.GetHeight());
}

void ImGuiDslEngine::RenderNode(const std::shared_ptr<compiler::AstNode>& node, float dt) {
    if (!node) return;

    switch (node->type) {
        case compiler::BinaryNodeType::Dock: {
            in_dock_context_ = true;
            dock_button_index_ = 0;
            int pad = (width_ >= 500 && height_ >= 90) ? 16 : 0;
            int dock_h = height_ - 2 * pad;
            int icon_size = 46;
            int start_y = pad + (dock_h - icon_size) / 2;

            // Compute total width of dock children to center them inside dock body
            float total_w = 46.0f * 7.0f + 16.0f + 85.0f + 8.0f * 10.0f; // ~503px
            float start_x = std::max(static_cast<float>(pad + 16), (static_cast<float>(width_) - total_w) * 0.5f);

            ImGui::SetCursorPos(ImVec2(start_x, static_cast<float>(start_y)));
            for (const auto& child : node->children) {
                RenderNode(child, dt);
            }
            in_dock_context_ = false;
            break;
        }
        case compiler::BinaryNodeType::TopBar: {
            in_topbar_context_ = true;
            topbar_spacer_count_ = 0;
            ImGui::SetCursorPos(ImVec2(18.0f, 5.0f));
            for (const auto& child : node->children) {
                RenderNode(child, dt);
            }
            in_topbar_context_ = false;
            break;
        }
        case compiler::BinaryNodeType::HStack: {
            for (size_t i = 0; i < node->children.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine(0.0f, node->spacing);
                }
                RenderNode(node->children[i], dt);
            }
            break;
        }
        case compiler::BinaryNodeType::VStack: {
            for (const auto& child : node->children) {
                RenderNode(child, dt);
            }
            break;
        }
        case compiler::BinaryNodeType::Spacer: {
            if (in_topbar_context_) {
                topbar_spacer_count_++;
                if (topbar_spacer_count_ == 1) {
                    // Center the upcoming Dynamic Island Clock Pill!
                    float pill_w = 184.0f;
                    float target_x = (static_cast<float>(width_) - pill_w) * 0.5f;
                    float cur_x = ImGui::GetCursorPosX();
                    float sp = std::max(8.0f, target_x - cur_x);
                    ImGui::Dummy(ImVec2(sp, 1.0f));
                } else if (topbar_spacer_count_ == 2) {
                    // Push remaining status items to the right side
                    float right_w = 236.0f;
                    float target_x = static_cast<float>(width_) - right_w;
                    float cur_x = ImGui::GetCursorPosX();
                    float sp = std::max(8.0f, target_x - cur_x);
                    ImGui::Dummy(ImVec2(sp, 1.0f));
                } else {
                    float sp = (node->numeric_value > 0.0f) ? node->numeric_value : 8.0f;
                    ImGui::Dummy(ImVec2(sp, 1.0f));
                }
            } else {
                float sp = (node->numeric_value > 0.0f) ? node->numeric_value : 8.0f;
                ImGui::Dummy(ImVec2(sp, 1.0f));
            }
            break;
        }
        case compiler::BinaryNodeType::Button: {
            std::string label = ResolveValue(node->text_value, node->slot_binding);
            float btn_w = 0.0f;
            float btn_h = 0.0f;

            if (node->number_props.count("width")) btn_w = static_cast<float>(node->number_props.at("width"));
            if (node->number_props.count("height")) btn_h = static_cast<float>(node->number_props.at("height"));

            if (in_dock_context_) {
                if (btn_w <= 0.0f) btn_w = 46.0f;
                if (btn_h <= 0.0f) btn_h = 46.0f;

                std::string lower = label;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

                bool is_launcher = (label == "田" || lower.find("launch") != std::string::npos);
                bool is_term     = (lower.find("term") != std::string::npos);
                bool is_files    = (lower.find("file") != std::string::npos);
                bool is_web      = (lower.find("web") != std::string::npos || lower.find("brows") != std::string::npos);
                bool is_code     = (lower.find("code") != std::string::npos || lower.find("edit") != std::string::npos);
                bool is_music    = (lower.find("music") != std::string::npos);
                bool is_pref     = (lower.find("pref") != std::string::npos || lower.find("sett") != std::string::npos);

                ImVec4 c(0.20f, 0.25f, 0.35f, 0.85f);
                if (is_launcher) c = ImVec4(0.25f, 0.28f, 0.38f, 0.70f);
                else if (is_files) c = ImVec4(0.00f, 0.48f, 1.00f, 0.95f);
                else if (is_term)  c = ImVec4(0.12f, 0.15f, 0.20f, 0.95f);
                else if (is_web)   c = ImVec4(0.19f, 0.78f, 0.35f, 0.95f);
                else if (is_code)  c = ImVec4(0.35f, 0.34f, 0.84f, 0.95f);
                else if (is_music) c = ImVec4(1.00f, 0.58f, 0.00f, 0.95f);
                else if (is_pref)  c = ImVec4(0.68f, 0.32f, 0.87f, 0.95f);

                ImGui::PushStyleColor(ImGuiCol_Button, c);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x * 1.15f, c.y * 1.15f, c.z * 1.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.0f);

                std::string btn_id = "##dock_btn_" + std::to_string(dock_button_index_);
                std::string display_label = label;
                if (is_launcher) display_label = "";
                else if (is_term) display_label = ">_";

                if (ImGui::Button(is_launcher ? btn_id.c_str() : display_label.c_str(), ImVec2(btn_w, btn_h))) {
                    if (action_callback_ && !node->action_value.empty()) {
                        action_callback_(node->action_value);
                    }
                }

                ImVec2 p_min = ImGui::GetItemRectMin();
                ImVec2 p_max = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();

                if (is_launcher) {
                    // Draw crisp 2x2 Launchpad grid tiles
                    float cx = (p_min.x + p_max.x) * 0.5f;
                    float cy = (p_min.y + p_max.y) * 0.5f;
                    uint32_t tile_col = 0xFFFFFFFF;
                    dl->AddRectFilled(ImVec2(cx - 10.0f, cy - 10.0f), ImVec2(cx - 2.0f, cy - 2.0f), tile_col, 2.0f);
                    dl->AddRectFilled(ImVec2(cx + 2.0f, cy - 10.0f), ImVec2(cx + 10.0f, cy - 2.0f), tile_col, 2.0f);
                    dl->AddRectFilled(ImVec2(cx - 10.0f, cy + 2.0f), ImVec2(cx - 2.0f, cy + 10.0f), tile_col, 2.0f);
                    dl->AddRectFilled(ImVec2(cx + 2.0f, cy + 2.0f), ImVec2(cx + 10.0f, cy + 10.0f), tile_col, 2.0f);
                }

                // Active app indicator: glowing white dot underneath
                if (!is_launcher && (dock_button_index_ == active_app_index_ + 1)) {
                    float dot_x = (p_min.x + p_max.x) * 0.5f;
                    float dot_y = p_max.y + 4.0f;
                    dl->AddCircleFilled(ImVec2(dot_x, dot_y), 2.5f, 0xFFFFFFFF);
                }

                dock_button_index_++;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
            } else {
                if (btn_h <= 0.0f) btn_h = 24.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.22f, 0.30f, 0.65f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.32f, 0.44f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.48f, 1.00f, 0.90f));

                if (ImGui::Button(label.c_str(), ImVec2(btn_w, btn_h))) {
                    if (action_callback_ && !node->action_value.empty()) {
                        action_callback_(node->action_value);
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();
            }
            break;
        }
        case compiler::BinaryNodeType::Text: {
            std::string text = ResolveValue(node->text_value, node->slot_binding);
            ImGui::TextUnformatted(text.c_str());
            break;
        }
        case compiler::BinaryNodeType::Badge: {
            std::string text = ResolveValue(node->text_value, node->slot_binding);
            bool is_clock = (node->slot_binding == "clock_time" || node->slot_binding == "$clock_time");
            bool is_sys = (node->slot_binding == "sys_badge" || node->slot_binding == "$sys_badge");

            if (is_clock) {
                // macOS Dynamic Island Clock Pill
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.05f, 0.08f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.04f, 0.05f, 0.08f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.05f, 0.08f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

                ImGui::Button(text.c_str(), ImVec2(184.0f, 24.0f));

                // Draw top white pill handle accent & rim
                ImVec2 p_min = ImGui::GetItemRectMin();
                ImVec2 p_max = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                float cx = (p_min.x + p_max.x) * 0.5f;
                dl->AddRectFilled(ImVec2(cx - 20.0f, p_min.y + 2.0f), ImVec2(cx + 20.0f, p_min.y + 4.0f), 0xCCFFFFFF, 1.0f);
                dl->AddRect(p_min, p_max, 0x33FFFFFF, 12.0f, 0, 1.0f);

                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
            } else if (is_sys) {
                // Prism Brand Capsule: Red badge with emblem & PRISM text
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.22f, 0.22f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.22f, 0.22f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.22f, 0.22f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

                ImGui::Button(text.c_str(), ImVec2(0.0f, 24.0f));

                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
            } else {
                // Status Pills: Wi-Fi, Battery, Active Count
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.22f, 0.30f, 0.65f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.22f, 0.30f, 0.65f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.22f, 0.30f, 0.65f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.97f, 1.00f, 1.0f));

                ImGui::Button(text.c_str(), ImVec2(0.0f, 22.0f));

                // Add subtle 1px border
                ImVec2 p_min = ImGui::GetItemRectMin();
                ImVec2 p_max = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(p_min, p_max, 0x22FFFFFF, 7.0f, 0, 1.0f);

                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
            }
            break;
        }
        default: {
            // Render children recursively for any container or layout node
            for (const auto& child : node->children) {
                RenderNode(child, dt);
            }
            break;
        }
    }
}

} // namespace prism::gui
