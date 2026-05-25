#include "map_inspection.h"

#include "app_utils.h"
#include "dataset_library.h"
#include "feature_props.h"
#include "imgui.h"
#include "layer_geometry.h"
#include "parcel_unified.h"
#include "real_property_ui.h"
#include "stb_image.h"
#include "worldsim_app_internal.h"

#include <filesystem>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
namespace fs = std::filesystem;

const PolygonGeometryArtifact* polygonArtifactForLayer(const MapInspectionContext& ctx, size_t layer_idx) {
    if (!ctx.polygon_geometry_artifacts) return nullptr;
    auto it = ctx.polygon_geometry_artifacts->find(layer_idx);
    if (it == ctx.polygon_geometry_artifacts->end()) return nullptr;
    return &it->second;
}

bool pointInTriangleLonLat(
    float px, float py,
    const ImVec2& a,
    const ImVec2& b,
    const ImVec2& c) {
    auto cross = [](const ImVec2& u, const ImVec2& v, float x, float y) {
        return (v.x - u.x) * (y - u.y) - (v.y - u.y) * (x - u.x);
    };
    const float c1 = cross(a, b, px, py);
    const float c2 = cross(b, c, px, py);
    const float c3 = cross(c, a, px, py);
    const bool has_neg = (c1 < 0.0f) || (c2 < 0.0f) || (c3 < 0.0f);
    const bool has_pos = (c1 > 0.0f) || (c2 > 0.0f) || (c3 > 0.0f);
    return !(has_neg && has_pos);
}

bool pointInPolygonArtifactFeature(
    const PolygonGeometryArtifact& artifact,
    size_t feature_idx,
    float lon,
    float lat) {
    if (feature_idx >= artifact.features.size()) return false;
    const GeometryArtifactFeatureRecord& rec = artifact.features[feature_idx];
    const uint32_t end = rec.index_offset + rec.index_count;
    if (end > artifact.fill_indices.size()) return false;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = artifact.fill_indices[i];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) continue;
        if (pointInTriangleLonLat(lon, lat, artifact.vertices[ia], artifact.vertices[ib], artifact.vertices[ic])) {
            return true;
        }
    }
    return false;
}

enum class PointMarkerGlyph {
    Circle,
    Square,
    Diamond,
    Triangle,
    Plus,
    Cross,
    Droplet
};

bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string hs = haystack;
    std::string nd = needle;
    std::transform(hs.begin(), hs.end(), hs.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(nd.begin(), nd.end(), nd.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return hs.find(nd) != std::string::npos;
}

std::string pointFeatureTitle(const LayerDef::FeatureRecord& fg);
std::string eventFeatureOpenUrl(const LayerDef::FeatureRecord& fg);

const char* pointIconTypeLabel(const LayerDef& layer, const LayerDef::FeatureRecord* fg = nullptr) {
    if (fg && isLikelyCrimePointLayer(layer)) return crimePointTypeLabel(*fg);
    if (containsCaseInsensitive(layer.name, "water")) return "Waterpoint";
    if (containsCaseInsensitive(layer.name, "health")) return "Health facility";
    if (containsCaseInsensitive(layer.name, "school")) return "School";
    if (containsCaseInsensitive(layer.name, "market")) return "Market";
    if (containsCaseInsensitive(layer.name, "police")) return "Police station";
    if (containsCaseInsensitive(layer.name, "church")) return "Church";
    if (containsCaseInsensitive(layer.name, "industry")) return "Industry";
    if (containsCaseInsensitive(layer.name, "filling")) return "Filling station";
    if (containsCaseInsensitive(layer.name, "event") ||
        containsCaseInsensitive(layer.subcategory, "event") ||
        containsCaseInsensitive(layer.duckdb_role, "point_event")) {
        return "Location event";
    }
    if (containsCaseInsensitive(layer.name, "settlement")) return "Settlement";
    switch (layer.category) {
        case LayerDef::Category::PublicHealth: return "Public health point";
        case LayerDef::Category::Infrastructure: return "Infrastructure point";
        case LayerDef::Category::Safety: return "Safety point";
        case LayerDef::Category::Zoning: return "Zoning point";
        case LayerDef::Category::Housing:
        default: return "Point feature";
    }
}

PointMarkerGlyph pointMarkerGlyphForLayer(const LayerDef& layer, const LayerDef::FeatureRecord* fg = nullptr) {
    if (fg && isLikelyCrimePointLayer(layer)) {
        return static_cast<PointMarkerGlyph>(crimePointGlyphCode(*fg));
    }
    if (containsCaseInsensitive(layer.name, "water")) return PointMarkerGlyph::Droplet;
    if (containsCaseInsensitive(layer.name, "health")) return PointMarkerGlyph::Cross;
    if (containsCaseInsensitive(layer.name, "school")) return PointMarkerGlyph::Triangle;
    if (containsCaseInsensitive(layer.name, "market")) return PointMarkerGlyph::Diamond;
    if (containsCaseInsensitive(layer.name, "police")) return PointMarkerGlyph::Diamond;
    if (containsCaseInsensitive(layer.name, "church")) return PointMarkerGlyph::Plus;
    if (containsCaseInsensitive(layer.name, "industry")) return PointMarkerGlyph::Square;
    if (containsCaseInsensitive(layer.name, "filling")) return PointMarkerGlyph::Square;
    if (containsCaseInsensitive(layer.name, "event") ||
        containsCaseInsensitive(layer.subcategory, "event") ||
        containsCaseInsensitive(layer.duckdb_role, "point_event")) {
        return PointMarkerGlyph::Droplet;
    }
    switch (layer.category) {
        case LayerDef::Category::PublicHealth: return PointMarkerGlyph::Cross;
        case LayerDef::Category::Infrastructure: return PointMarkerGlyph::Square;
        case LayerDef::Category::Safety: return PointMarkerGlyph::Diamond;
        case LayerDef::Category::Zoning: return PointMarkerGlyph::Triangle;
        case LayerDef::Category::Housing:
        default: return PointMarkerGlyph::Circle;
    }
}

void drawPointMarkerGlyph(ImDrawList* draw, PointMarkerGlyph glyph, const ImVec2& center, float radius, ImU32 color) {
    const float outline_thickness = std::max(1.0f, radius * 0.26f);
    const ImU32 outline = IM_COL32(18, 22, 26, 235);
    switch (glyph) {
        case PointMarkerGlyph::Square: {
            const ImVec2 a(center.x - radius, center.y - radius);
            const ImVec2 b(center.x + radius, center.y + radius);
            draw->AddRectFilled(a, b, color, 1.5f);
            draw->AddRect(a, b, outline, 1.5f, 0, outline_thickness);
            break;
        }
        case PointMarkerGlyph::Diamond: {
            ImVec2 pts[4] = {
                ImVec2(center.x, center.y - radius),
                ImVec2(center.x + radius, center.y),
                ImVec2(center.x, center.y + radius),
                ImVec2(center.x - radius, center.y)
            };
            draw->AddConvexPolyFilled(pts, 4, color);
            draw->AddPolyline(pts, 4, outline, ImDrawFlags_Closed, outline_thickness);
            break;
        }
        case PointMarkerGlyph::Triangle: {
            const float h = radius * 1.15f;
            ImVec2 pts[3] = {
                ImVec2(center.x, center.y - h),
                ImVec2(center.x + radius, center.y + radius * 0.8f),
                ImVec2(center.x - radius, center.y + radius * 0.8f)
            };
            draw->AddConvexPolyFilled(pts, 3, color);
            draw->AddPolyline(pts, 3, outline, ImDrawFlags_Closed, outline_thickness);
            break;
        }
        case PointMarkerGlyph::Plus: {
            draw->AddCircleFilled(center, radius, IM_COL32(245, 248, 250, 245), 24);
            draw->AddCircle(center, radius, outline, 24, outline_thickness);
            const float arm = radius * 0.62f;
            draw->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x + arm, center.y), color, outline_thickness + 0.6f);
            draw->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y + arm), color, outline_thickness + 0.6f);
            break;
        }
        case PointMarkerGlyph::Cross: {
            draw->AddCircleFilled(center, radius, IM_COL32(245, 248, 250, 245), 24);
            draw->AddCircle(center, radius, outline, 24, outline_thickness);
            const float arm = radius * 0.58f;
            draw->AddLine(
                ImVec2(center.x - arm, center.y - arm),
                ImVec2(center.x + arm, center.y + arm),
                color,
                outline_thickness + 0.6f);
            draw->AddLine(
                ImVec2(center.x - arm, center.y + arm),
                ImVec2(center.x + arm, center.y - arm),
                color,
                outline_thickness + 0.6f);
            break;
        }
        case PointMarkerGlyph::Droplet: {
            ImVec2 pts[5] = {
                ImVec2(center.x, center.y - radius * 1.22f),
                ImVec2(center.x + radius * 0.82f, center.y - radius * 0.1f),
                ImVec2(center.x + radius * 0.46f, center.y + radius * 0.92f),
                ImVec2(center.x - radius * 0.46f, center.y + radius * 0.92f),
                ImVec2(center.x - radius * 0.82f, center.y - radius * 0.1f)
            };
            draw->AddConvexPolyFilled(pts, 5, color);
            draw->AddPolyline(pts, 5, outline, ImDrawFlags_Closed, outline_thickness);
            break;
        }
        case PointMarkerGlyph::Circle:
        default:
            draw->AddCircleFilled(center, radius, color, 24);
            draw->AddCircle(center, radius, outline, 24, outline_thickness);
            break;
    }
}

struct HoverImageCacheEntry {
    TileTexture tex;
    int width = 0;
    int height = 0;
    std::atomic<bool> download_requested{false};
    std::atomic<bool> downloading{false};
    std::atomic<bool> download_complete{false};
    std::atomic<bool> load_attempted{false};
    std::atomic<bool> available{false};
};

bool isLikelyEventPointLayer(const LayerDef& layer) {
    return containsCaseInsensitive(layer.name, "event") ||
           containsCaseInsensitive(layer.subcategory, "event") ||
           containsCaseInsensitive(layer.duckdb_role, "point_event");
}

std::string hoverImageCachePathForUrl(const std::string& url) {
    if (url.empty()) return "";
    std::string ext = ".img";
    const size_t q = url.find('?');
    const std::string base = url.substr(0, q);
    const size_t slash = base.find_last_of('/');
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        ext = base.substr(dot);
        if (ext.size() > 8) ext = ".img";
    }
    const size_t h = std::hash<std::string>{}(url);
    fs::path dir = fs::temp_directory_path() / "worldsim3_hover_images";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / (std::to_string(h) + ext)).string();
}

HoverImageCacheEntry* getHoverImage(const std::string& url) {
    static std::unordered_map<std::string, std::shared_ptr<HoverImageCacheEntry>> cache;
    static std::mutex cache_mutex;
    if (url.empty()) return nullptr;
    std::shared_ptr<HoverImageCacheEntry> entry;
    {
        std::lock_guard<std::mutex> lk(cache_mutex);
        auto it = cache.find(url);
        if (it == cache.end()) {
            it = cache.emplace(url, std::make_shared<HoverImageCacheEntry>()).first;
        }
        entry = it->second;
    }
    const std::string cache_path = hoverImageCachePathForUrl(url);
    if (cache_path.empty()) return nullptr;
    if (!entry->download_requested.exchange(true)) {
        std::error_code ec;
        if (fs::exists(cache_path, ec) && !ec) {
            entry->download_complete.store(true);
        } else {
            entry->downloading.store(true);
            std::thread([url, cache_path, entry]() {
                std::string err;
                const bool ok = downloadUrlToFile(url, cache_path, err);
                entry->downloading.store(false);
                entry->download_complete.store(ok);
            }).detach();
            return nullptr;
        }
    }
    if (!entry->download_complete.load()) return nullptr;
    if (entry->available.load()) return entry.get();
    if (entry->load_attempted.exchange(true)) return nullptr;

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(cache_path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }
    if (!uploadRgbaTexture(pixels, (uint32_t)w, (uint32_t)h, entry->tex)) {
        stbi_image_free(pixels);
        return nullptr;
    }
    stbi_image_free(pixels);
    entry->width = w;
    entry->height = h;
    entry->available.store(true);
    return entry.get();
}

std::string eventFeatureImageUrl(const LayerDef::FeatureRecord& fg) {
    std::string url = firstDisplayProperty(
        fg,
        {"image_url_resolved", "org_image_url_resolved", "imageUrl", "image_url", "orgImageUrl"});
    if (!url.empty() && url[0] == '/') return std::string("https://codecollective.us") + url;
    return url;
}

bool samePointLocation(const LayerDef::FeatureRecord& a, const LayerDef::FeatureRecord& b) {
    constexpr double eps = 1e-7;
    return std::abs((double)a.extent.min_lon - (double)b.extent.min_lon) <= eps &&
           std::abs((double)a.extent.min_lat - (double)b.extent.min_lat) <= eps;
}

std::vector<const LayerDef::FeatureRecord*> collectColocatedEventFeatures(
    const LayerDef& layer,
    const LayerDef::FeatureRecord& anchor) {
    std::vector<const LayerDef::FeatureRecord*> matches;
    matches.reserve(8);
    for (const auto& fg : layer.features) {
        if (samePointLocation(fg, anchor)) matches.push_back(&fg);
    }
    return matches;
}

void drawEventAvatar(const LayerDef::FeatureRecord& fg, float max_w, float max_h) {
    const std::string image_url = eventFeatureImageUrl(fg);
    if (HoverImageCacheEntry* image = getHoverImage(image_url); image && image->tex.descriptor != VK_NULL_HANDLE) {
        float draw_w = (float)image->width;
        float draw_h = (float)image->height;
        const float scale = std::min(max_w / std::max(1.0f, draw_w), max_h / std::max(1.0f, draw_h));
        if (scale < 1.0f) {
            draw_w *= scale;
            draw_h *= scale;
        }
        ImGui::Image((ImTextureID)image->tex.descriptor, ImVec2(draw_w, draw_h));
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(max_w, max_h);
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(42, 48, 56, 255), 4.0f);
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(88, 98, 112, 255), 4.0f, 0, 1.0f);
    ImGui::Dummy(size);
}

void drawEventListEntry(const LayerDef::FeatureRecord& fg, const char* row_id, bool clickable) {
    const std::string title = pointFeatureTitle(fg);
    const std::string org_name = firstDisplayProperty(fg, {"org_name", "orgName", "organization", "source_group"});
    const std::string start = firstDisplayProperty(fg, {"startDate", "start_date", "date_start"});
    const std::string venue = firstDisplayProperty(fg, {"location.name", "venue", "Venue"});
    const float row_height = 40.0f;
    const float avail_w = std::max(220.0f, ImGui::GetContentRegionAvail().x);

    ImGui::PushID(row_id);
    if (clickable) {
        ImGui::InvisibleButton("event_row", ImVec2(avail_w, row_height));
    } else {
        ImGui::Dummy(ImVec2(avail_w, row_height));
    }
    const bool hovered = clickable && ImGui::IsItemHovered();
    const bool clicked = clickable && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 bg = hovered ? IM_COL32(58, 88, 132, 180) : IM_COL32(32, 36, 44, 120);
    draw->AddRectFilled(min, max, bg, 6.0f);
    draw->AddRect(min, max, IM_COL32(78, 86, 98, 180), 6.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(min.x + 6.0f, min.y + 6.0f));
    drawEventAvatar(fg, 28.0f, 28.0f);
    ImGui::SetCursorScreenPos(ImVec2(min.x + 42.0f, min.y + 5.0f));
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", title.empty() ? "(untitled event)" : title.c_str());
    if (!org_name.empty()) {
        ImGui::TextDisabled("%s", org_name.c_str());
    } else if (!venue.empty()) {
        ImGui::TextDisabled("%s", venue.c_str());
    } else if (!start.empty()) {
        ImGui::TextDisabled("%s", start.c_str());
    }
    ImGui::EndGroup();
    if (clicked) {
        const std::string open_url = eventFeatureOpenUrl(fg);
        if (!open_url.empty()) openUrlInBrowser(open_url);
    }
    ImGui::PopID();
}

void drawPointLayerHeader(const LayerDef& layer, const LayerDef::FeatureRecord* fg = nullptr) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float icon_size = 22.0f;
    ImGui::Dummy(ImVec2(icon_size, icon_size));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    drawPointMarkerGlyph(
        draw,
        pointMarkerGlyphForLayer(layer, fg),
        center,
        7.0f,
        ImGui::ColorConvertFloat4ToU32(layer.color));
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextWrapped("%s", layer.name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s", pointIconTypeLabel(layer, fg));
    ImGui::EndGroup();
}

void drawFeatureProperties(const char* title, const LayerDef::FeatureRecord& fg) {
    ImGui::TextUnformatted(title);
    const FeaturePropertyPairs* props = getPropertyPairs(fg);
    if (!props) return;
    for (const auto& kv : *props) {
        std::string v = trimDisplayValue(kv.second);
        if (v.empty()) continue;
        ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
    }
}

std::string pointFeatureTitle(const LayerDef::FeatureRecord& fg) {
    return firstDisplayProperty(
        fg,
        {"name", "Name", "NAME", "facility_name", "facility_n", "school_name", "school_nam",
         "market_name", "market_nam", "settlement_name", "settlement", "church_name",
         "industry_name", "station_name", "waterpoint_name"});
}

std::string eventFeatureOpenUrl(const LayerDef::FeatureRecord& fg) {
    std::string url = firstDisplayProperty(fg, {"url", "URL"});
    if (!url.empty()) return url;
    return firstDisplayProperty(fg, {"source_url", "source", "Source"});
}

std::string pointFeatureOpenUrl(const LayerDef& layer, const LayerDef::FeatureRecord& fg) {
    if (!isLikelyEventPointLayer(layer)) return {};
    return eventFeatureOpenUrl(fg);
}

void drawEventPointSummary(const LayerDef& layer, const LayerDef::FeatureRecord& fg) {
    const std::vector<const LayerDef::FeatureRecord*> colocated = collectColocatedEventFeatures(layer, fg);
    const std::string title = pointFeatureTitle(fg);
    const std::string description = firstDisplayProperty(fg, {"description", "Description", "DESC"});
    const std::string address = firstDisplayProperty(
        fg, {"location.address", "address", "Address", "ADDRESS", "addr", "ADDR"});
    const std::string venue = firstDisplayProperty(fg, {"location.name", "venue", "Venue"});
    const std::string org_name = firstDisplayProperty(fg, {"org_name", "orgName", "organization", "source_group"});
    const std::string status = firstDisplayProperty(fg, {"status", "Status"});
    const std::string start = firstDisplayProperty(fg, {"startDate", "start_date", "date_start"});
    const std::string end_date = firstDisplayProperty(fg, {"endDate", "end_date"});
    const std::string end_time = firstDisplayProperty(fg, {"endTime", "end_time"});
    const std::string tags = firstDisplayProperty(fg, {"tags", "Tags"});
    const std::string event_url = firstDisplayProperty(fg, {"url", "URL"});
    const std::string source_url = firstDisplayProperty(fg, {"source_url", "source", "Source"});
    const std::string image_url = eventFeatureImageUrl(fg);

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(540.0f);
    drawPointLayerHeader(layer, &fg);
    if (colocated.size() > 1) {
        ImGui::TextWrapped("%zu events at this location", colocated.size());
        ImGui::TextDisabled("Click the marker to open the full event list.");
        ImGui::Separator();
        ImGui::BeginChild("event_location_stack", ImVec2(0.0f, std::min(300.0f, 48.0f * (float)colocated.size())), true);
        for (size_t i = 0; i < colocated.size(); ++i) {
            drawEventListEntry(*colocated[i], ("hover_row_" + std::to_string(i)).c_str(), false);
        }
        ImGui::EndChild();
        ImGui::TextWrapped("Location: %.6f, %.6f", fg.extent.min_lat, fg.extent.min_lon);
    } else {
        if (HoverImageCacheEntry* image = getHoverImage(image_url); image && image->tex.descriptor != VK_NULL_HANDLE) {
            const float max_w = 240.0f;
            const float max_h = 140.0f;
            float draw_w = (float)image->width;
            float draw_h = (float)image->height;
            const float scale = std::min(max_w / std::max(1.0f, draw_w), max_h / std::max(1.0f, draw_h));
            if (scale < 1.0f) {
                draw_w *= scale;
                draw_h *= scale;
            }
            ImGui::Image((ImTextureID)image->tex.descriptor, ImVec2(draw_w, draw_h));
        }
        if (!title.empty()) ImGui::TextWrapped("%s", title.c_str());
        if (!org_name.empty()) ImGui::TextDisabled("%s", org_name.c_str());
        if (!status.empty()) ImGui::TextWrapped("Status: %s", status.c_str());
        if (!start.empty()) ImGui::TextWrapped("Starts: %s", start.c_str());
        if (!end_date.empty()) ImGui::TextWrapped("Ends: %s", end_date.c_str());
        if (!end_time.empty()) ImGui::TextWrapped("End Time: %s", end_time.c_str());
        if (!venue.empty()) ImGui::TextWrapped("Venue: %s", venue.c_str());
        if (!address.empty()) ImGui::TextWrapped("Address: %s", address.c_str());
        if (!tags.empty()) ImGui::TextWrapped("Tags: %s", tags.c_str());
        ImGui::TextWrapped("Location: %.6f, %.6f", fg.extent.min_lat, fg.extent.min_lon);
        if (!description.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", description.c_str());
        }
        if (!event_url.empty() || !source_url.empty() || !image_url.empty()) {
            ImGui::Separator();
            if (!event_url.empty()) ImGui::TextWrapped("Event URL: %s", event_url.c_str());
            if (!source_url.empty()) ImGui::TextWrapped("Source: %s", source_url.c_str());
            if (!image_url.empty()) ImGui::TextWrapped("Image: %s", image_url.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void drawPointFeatureSummary(const LayerDef& layer, const LayerDef::FeatureRecord& fg) {
    if (isLikelyEventPointLayer(layer)) {
        drawEventPointSummary(layer, fg);
        return;
    }
    const std::string title = pointFeatureTitle(fg);
    const std::string lga = firstDisplayProperty(fg, {"lga_name", "LGA_NAME", "lga", "LGA"});
    const std::string ward = firstDisplayProperty(fg, {"ward_name", "WARD_NAME", "ward", "WARD"});
    const std::string address = firstDisplayProperty(fg, {"address", "Address", "ADDRESS", "addr", "ADDR"});
    const std::string feature_type = firstDisplayProperty(
        fg, {"type", "Type", "TYPE", "category", "Category", "CATEGORY", "subtype", "SUBTYPE"});

    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(440.0f);
    drawPointLayerHeader(layer, &fg);
    if (!title.empty()) ImGui::TextWrapped("Feature: %s", title.c_str());
    if (!feature_type.empty()) ImGui::TextWrapped("Type: %s", feature_type.c_str());
    if (!lga.empty()) ImGui::TextWrapped("LGA: %s", lga.c_str());
    if (!ward.empty()) ImGui::TextWrapped("Ward: %s", ward.c_str());
    if (!address.empty()) ImGui::TextWrapped("Address: %s", address.c_str());
    ImGui::TextWrapped("Location: %.6f, %.6f", fg.extent.min_lat, fg.extent.min_lon);
    ImGui::Separator();
    drawFeatureProperties("All Feature Fields", fg);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
}

ParcelHoverResolution resolveHoveredParcel(const MapInspectionContext& ctx) {
    ParcelHoverResolution out;
    if (!ctx.hover_state || !ctx.layers) return out;
    out.layer_idx = ctx.hover_state->hovered_parcel_layer_idx;
    out.feature_idx = ctx.hover_state->hovered_parcel_idx;
    out.hit = out.feature_idx != (size_t)-1;
    if (!out.hit) return out;
    out.entity_id =
        !ctx.hover_state->hovered_parcel_entity_id.empty()
            ? ctx.hover_state->hovered_parcel_entity_id
            : (out.layer_idx >= 0 &&
               (size_t)out.layer_idx < ctx.layers->size() &&
               out.feature_idx < (*ctx.layers)[(size_t)out.layer_idx].features.size())
                ? featureEntityIdForLayerFeature(
                    (*ctx.layers)[(size_t)out.layer_idx],
                    (*ctx.layers)[(size_t)out.layer_idx].features[out.feature_idx],
                    out.feature_idx)
                : std::string();
    out.unified_record =
        (ctx.unified_parcels && !out.entity_id.empty())
            ? unifiedParcelAt(*ctx.unified_parcels, out.entity_id)
            : nullptr;
    return out;
}

ParcelHoverDetail resolveParcelHoverDetail(const MapInspectionContext& ctx, const ParcelHoverResolution& hovered) {
    ParcelHoverDetail out;
    if (!hovered.hit || hovered.entity_id.empty()) return out;
    out.parcel_entity_id = hovered.entity_id;

    if (hovered.unified_record) {
        const UnifiedParcelRecord& row = *hovered.unified_record;
        out.available = true;
        out.blocklot = row.blocklot;
        out.owner = row.owner;
        out.owner_display = row.owner_display;
        out.address = row.address;
        out.zipcode = row.zip;
        out.status = row.status;
        out.parcel_has_geometry = row.parcel_has_geometry;
        out.has_property_record = row.has_property_record;
        out.parcel_extent = row.parcel_extent;
        out.vacant_notice_count = row.vacant_notice_count;
        out.vacant_rehab_count = row.vacant_rehab_count;
        out.tax_lien_count = row.tax_lien_count;
        out.tax_sale_count = row.tax_sale_count;
        out.tax_lien_amount = row.tax_lien_amount;
        out.tax_sale_amount = row.tax_sale_amount;
        return out;
    }

    if (!ctx.duckdb_analytics || !ctx.duckdb_analytics->status().last_rebuild_ok) return out;
    const DuckDbQueryResult detail = ctx.duckdb_analytics->queryUnifiedParcelDetail(hovered.entity_id);
    if (!detail.ok || detail.rows.empty()) return out;
    const auto& row = detail.rows.front();
    auto cell = [&](const char* column) -> std::string {
        for (size_t i = 0; i < detail.columns.size() && i < row.size(); ++i) {
            if (detail.columns[i] == column) return row[i];
        }
        return {};
    };
    out.available = true;
    out.blocklot = cell("blocklot");
    out.owner = cell("owner");
    out.owner_display = cell("owner_display");
    out.address = cell("address");
    out.zipcode = cell("zipcode");
    out.status = cell("status");
    out.parcel_has_geometry = trimDisplayValue(cell("parcel_has_geometry")) == "true";
    out.has_property_record = trimDisplayValue(cell("has_property_record")) == "true";
    out.parcel_extent.min_lon = (float)parseNumericField(cell("min_lon"));
    out.parcel_extent.min_lat = (float)parseNumericField(cell("min_lat"));
    out.parcel_extent.max_lon = (float)parseNumericField(cell("max_lon"));
    out.parcel_extent.max_lat = (float)parseNumericField(cell("max_lat"));
    out.vacant_notice_count = (int)parseNumericField(cell("vacant_notice_count"));
    out.vacant_rehab_count = (int)parseNumericField(cell("vacant_rehab_count"));
    out.tax_lien_count = (int)parseNumericField(cell("tax_lien_count"));
    out.tax_sale_count = (int)parseNumericField(cell("tax_sale_count"));
    out.tax_lien_amount = parseNumericField(cell("tax_lien_amount"));
    out.tax_sale_amount = parseNumericField(cell("tax_sale_amount"));
    return out;
}

bool applyParcelClickSelection(const MapInspectionContext& ctx, const ParcelHoverResolution& hovered, bool ctrl_append) {
    if (!ctx.parcel_selection || !hovered.hit || hovered.entity_id.empty()) return false;
    if (!selectParcel(
            *ctx.parcel_selection,
            hovered.layer_idx,
            hovered.entity_id,
            ctrl_append)) {
        return false;
    }
    if (ctx.open_parcel_element) ctx.open_parcel_element(hovered.entity_id);
    if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = false;
    if (ctx.selected_zone_idx) *ctx.selected_zone_idx = (size_t)-1;
    return true;
}

void handleMapInspection(const MapInspectionContext& ctx) {
    static bool event_stack_popup_open = false;
    static int event_stack_layer_idx = -1;
    static float event_stack_lon = 0.0f;
    static float event_stack_lat = 0.0f;

    if (!ctx.hover_state || !ctx.layers || !ctx.parcel_selection) return;
    const ParcelHoverResolution hovered_parcel = resolveHoveredParcel(ctx);
    const int hovered_parcel_layer_idx = hovered_parcel.layer_idx;
    const size_t hovered_parcel_idx = hovered_parcel.feature_idx;
    const LayerDef::FeatureRecord* hovered_zone = ctx.hover_state->hovered_zone;
    const size_t hovered_zone_idx = ctx.hover_state->hovered_zone_idx;
    const LayerDef::FeatureRecord* hovered_point = ctx.hover_state->hovered_point;
    const int hovered_point_layer_idx = ctx.hover_state->hovered_point_layer_idx;
    const LayerDef::FeatureRecord* inspect_point = ctx.hover_state->inspect_point;
    const int inspect_point_layer_idx = ctx.hover_state->inspect_point_layer_idx;
    const bool hovered_parcel_hit = hovered_parcel.hit;
    const UnifiedParcelRecord* hovered_unified = hovered_parcel.unified_record;

    const bool click_select =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;

    if (ctx.map_hovered && click_select && inspect_point && inspect_point_layer_idx >= 0 &&
        (size_t)inspect_point_layer_idx < ctx.layers->size()) {
        const LayerDef& inspect_point_layer = (*ctx.layers)[(size_t)inspect_point_layer_idx];
        if (isLikelyEventPointLayer(inspect_point_layer)) {
            const std::vector<const LayerDef::FeatureRecord*> colocated =
                collectColocatedEventFeatures(inspect_point_layer, *inspect_point);
            if (colocated.size() > 1) {
                event_stack_popup_open = true;
                event_stack_layer_idx = inspect_point_layer_idx;
                event_stack_lon = inspect_point->extent.min_lon;
                event_stack_lat = inspect_point->extent.min_lat;
                ImGui::OpenPopup("Event Location List");
                hovered_zone = nullptr;
            } else {
                const std::string open_url = pointFeatureOpenUrl(inspect_point_layer, *inspect_point);
                if (!open_url.empty()) {
                    openUrlInBrowser(open_url);
                    return;
                }
            }
        } else {
            const std::string open_url = pointFeatureOpenUrl(inspect_point_layer, *inspect_point);
            if (!open_url.empty()) {
                openUrlInBrowser(open_url);
                return;
            }
        }
    }

    if (ctx.map_hovered && ctx.parcel_inspect_active && click_select && hovered_parcel_hit) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        applyParcelClickSelection(ctx, hovered_parcel, ctrl);
    } else if (ctx.map_hovered && ctx.zoning_inspect_active && click_select && hovered_zone != nullptr) {
        if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = true;
        if (ctx.selected_zone_idx) *ctx.selected_zone_idx = hovered_zone_idx;
        clearParcelSelection(*ctx.parcel_selection);
    }

    const ParcelHoverDetail hovered_detail = resolveParcelHoverDetail(ctx, hovered_parcel);
    if (ctx.parcel_hover_active && ctx.map_hovered && hovered_detail.available) {
        const std::string& blocklot_raw = hovered_detail.blocklot;
        const int vac_notice = hovered_detail.vacant_notice_count;
        const int vac_rehab = hovered_detail.vacant_rehab_count;
        const int tax_lien = hovered_detail.tax_lien_count;
        const int tax_sale = hovered_detail.tax_sale_count;
        const double tax_lien_amount = hovered_detail.tax_lien_amount;
        const double tax_sale_amount = hovered_detail.tax_sale_amount;
        const LayerDef::FeatureRecord* hovered_zoning = hovered_zone;
        if (hovered_detail.parcel_has_geometry &&
            ctx.zoning_layer_idx >= 0 && (size_t)ctx.zoning_layer_idx < ctx.layers->size()) {
            const LayerDef::FeatureExtent& parcel_extent = hovered_detail.parcel_extent;
            const float qlon = (parcel_extent.min_lon + parcel_extent.max_lon) * 0.5f;
            const float qlat = (parcel_extent.min_lat + parcel_extent.max_lat) * 0.5f;
            std::vector<uint32_t> zoning_candidates;
            bool have_zoning_candidates = false;
            if (ctx.layer_spatial && (size_t)ctx.zoning_layer_idx < ctx.layer_spatial->size() && (*ctx.layer_spatial)[(size_t)ctx.zoning_layer_idx].built) {
                have_zoning_candidates = queryLayerSpatialIndex(
                    (*ctx.layer_spatial)[(size_t)ctx.zoning_layer_idx], qlon, qlat, qlon, qlat, zoning_candidates);
            }
            if (have_zoning_candidates) {
                const auto& zfeats = (*ctx.layers)[(size_t)ctx.zoning_layer_idx].features;
                const PolygonGeometryArtifact* zoning_artifact =
                    polygonArtifactForLayer(ctx, (size_t)ctx.zoning_layer_idx);
                for (uint32_t zi : zoning_candidates) {
                    if (zi >= zfeats.size()) continue;
                    const auto& zf = zfeats[zi];
                    if (!zoning_artifact || (size_t)zi >= zoning_artifact->features.size()) continue;
                    const bool contains_point =
                        pointInPolygonArtifactFeature(*zoning_artifact, (size_t)zi, qlon, qlat);
                    if (qlon >= zf.extent.min_lon && qlon <= zf.extent.max_lon &&
                        qlat >= zf.extent.min_lat && qlat <= zf.extent.max_lat &&
                        contains_point) {
                        hovered_zoning = &zf;
                        break;
                    }
                }
            }
        }

        ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(440.0f);
        ImGui::TextUnformatted("Parcel Details");
        ImGui::Separator();
        ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
        ImGui::Text("Vacant Notices: %d", vac_notice);
        ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
        ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
        if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: %s", formatUsd(tax_lien_amount, 2).c_str());
        ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
        if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: %s", formatUsd(tax_sale_amount, 2).c_str());

        const LayerDef::FeatureRecord* hovered_rp =
            (hovered_unified && ctx.layers)
                ? unifiedRealPropertyGeometry(*hovered_unified, *ctx.layers)
                : nullptr;
        if (!hovered_rp && ctx.layers) {
            hovered_rp = resolveRealPropertyForBlocklot(
                *ctx.layers,
                ctx.real_property_layer_idx,
                ctx.real_property_by_blocklot,
                blocklot_raw);
        }
        drawRealPropertySummary(hovered_rp);

        ImGui::Separator();
        if (hovered_zoning && ctx.zoning_metadata) {
            std::string zone_key = zoningClassKey(*hovered_zoning);
            std::string zone_label = zoningClassLabel(*hovered_zoning);
            auto meta_it = ctx.zoning_metadata->find(zone_key);
            if (meta_it != ctx.zoning_metadata->end() && !meta_it->second.label.empty()) zone_label = meta_it->second.label;
            std::string zone_description = zoningDescription(*hovered_zoning, *ctx.zoning_metadata);
            const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextWrapped("%s", display_zone);
            ImGui::SetWindowFontScale(1.0f);
            if (!zone_description.empty()) ImGui::TextWrapped("%s", zone_description.c_str());
        } else if (ctx.zoning_layer_idx >= 0) {
            ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Open the parcel details panel for full parcel and property fields.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    } else if (ctx.parcel_hover_active && ctx.map_hovered && hovered_parcel_hit) {
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(300.0f);
        ImGui::TextUnformatted("Parcel Details");
        ImGui::Separator();
        ImGui::TextDisabled("Loading...");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (ctx.zoning_hover_active && ctx.map_hovered &&
        !(ctx.parcel_hover_active && hovered_parcel_hit) &&
        hovered_zone && ctx.zoning_metadata) {
        drawZoningHoverTooltip(*hovered_zone, *ctx.zoning_metadata);
    } else if (ctx.map_hovered && hovered_point && hovered_point_layer_idx >= 0 &&
               (size_t)hovered_point_layer_idx < ctx.layers->size()) {
        drawPointFeatureSummary((*ctx.layers)[(size_t)hovered_point_layer_idx], *hovered_point);
    }

    if (event_stack_popup_open) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_Appearing);
        const bool keep_open = ImGui::BeginPopupModal("Event Location List", &event_stack_popup_open, ImGuiWindowFlags_NoSavedSettings);
        if (keep_open) {
            if (event_stack_layer_idx >= 0 && (size_t)event_stack_layer_idx < ctx.layers->size()) {
                const LayerDef& layer = (*ctx.layers)[(size_t)event_stack_layer_idx];
                LayerDef::FeatureRecord anchor;
                anchor.extent.min_lon = event_stack_lon;
                anchor.extent.min_lat = event_stack_lat;
                const std::vector<const LayerDef::FeatureRecord*> colocated = collectColocatedEventFeatures(layer, anchor);
                drawPointLayerHeader(layer, colocated.empty() ? nullptr : colocated.front());
                ImGui::Separator();
                ImGui::TextWrapped("%zu events at %.6f, %.6f", colocated.size(), event_stack_lat, event_stack_lon);
                ImGui::BeginChild("event_location_click_list", ImVec2(0.0f, -38.0f), true);
                for (size_t i = 0; i < colocated.size(); ++i) {
                    drawEventListEntry(*colocated[i], ("popup_row_" + std::to_string(i)).c_str(), true);
                }
                ImGui::EndChild();
            }
            if (ImGui::Button("Close")) {
                event_stack_popup_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}
