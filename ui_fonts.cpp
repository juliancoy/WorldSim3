#include "ui_fonts.h"

#include "imgui.h"

#include <cstdio>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

namespace {
ImFont* g_map_title_font = nullptr;

const char* firstExistingPath(const std::initializer_list<const char*>& paths) {
    for (const char* path : paths) {
        std::error_code ec;
        if (path && fs::exists(path, ec) && !ec) return path;
    }
    return nullptr;
}
}

void configureWorldsimFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_map_title_font = nullptr;

    ImFontConfig base_config;
    base_config.OversampleH = 2;
    base_config.OversampleV = 1;
    base_config.RasterizerMultiply = 1.05f;

    constexpr float font_px = 16.5f;
    const char* base_font = firstExistingPath({
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuSans[wdth,wght].ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    });

    ImFont* loaded = nullptr;
    if (base_font) {
        loaded = io.Fonts->AddFontFromFileTTF(base_font, font_px, &base_config, io.Fonts->GetGlyphRangesDefault());
    }
    if (!loaded) {
        loaded = io.Fonts->AddFontDefault(&base_config);
        std::fprintf(stderr, "[worldsim3] UI font fallback: using ImGui default font\n");
    }
    g_map_title_font = loaded;

    const char* chinese_font = firstExistingPath({
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
    });
    if (chinese_font) {
        ImFontConfig cjk_config;
        cjk_config.MergeMode = true;
        if (std::strstr(chinese_font, "NotoSansCJK-Regular.ttc")) cjk_config.FontNo = 2;
        cjk_config.OversampleH = 1;
        cjk_config.OversampleV = 1;
        cjk_config.RasterizerMultiply = 1.05f;
        if (!io.Fonts->AddFontFromFileTTF(chinese_font, font_px, &cjk_config, io.Fonts->GetGlyphRangesChineseFull())) {
            std::fprintf(stderr, "[worldsim3] UI font warning: failed to merge Chinese font %s\n", chinese_font);
        }
    } else {
        std::fprintf(stderr, "[worldsim3] UI font warning: no Chinese-capable fallback font found\n");
    }

    if (const char* title_font = firstExistingPath({"./nofile"})) {
        ImFontConfig title_config;
        title_config.OversampleH = 2;
        title_config.OversampleV = 1;
        title_config.RasterizerMultiply = 1.05f;
        if (ImFont* loaded_title = io.Fonts->AddFontFromFileTTF(title_font, 48.0f, &title_config, io.Fonts->GetGlyphRangesDefault())) {
            g_map_title_font = loaded_title;
        } else {
            std::fprintf(stderr, "[worldsim3] UI font warning: failed to load title font %s\n", title_font);
        }
    } else {
        std::fprintf(stderr, "[worldsim3] UI font warning: no title font asset found at ./nofile\n");
    }

    io.FontDefault = loaded;
}

ImFont* getWorldsimMapTitleFont() {
    return g_map_title_font;
}
