#include "map_inspection.h"

#include "app_utils.h"
#include "dataset_library.h"
#include "feature_props.h"
#include "imgui.h"
#include "layer_geometry.h"
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

std::string pointFeatureTitle(const LayerDef::FeatureGeom& fg);
std::string eventFeatureOpenUrl(const LayerDef::FeatureGeom& fg);

const char* pointIconTypeLabel(const LayerDef& layer, const LayerDef::FeatureGeom* fg = nullptr) {
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

PointMarkerGlyph pointMarkerGlyphForLayer(const LayerDef& layer, const LayerDef::FeatureGeom* fg = nullptr) {
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

std::string eventFeatureImageUrl(const LayerDef::FeatureGeom& fg) {
    std::string url = firstDisplayProperty(
        fg,
        {"image_url_resolved", "org_image_url_resolved", "imageUrl", "image_url", "orgImageUrl"});
    if (!url.empty() && url[0] == '/') return std::string("https://codecollective.us") + url;
    return url;
}

bool samePointLocation(const LayerDef::FeatureGeom& a, const LayerDef::FeatureGeom& b) {
    constexpr double eps = 1e-7;
    return std::abs((double)a.extent.min_lon - (double)b.extent.min_lon) <= eps &&
           std::abs((double)a.extent.min_lat - (double)b.extent.min_lat) <= eps;
}

std::vector<const LayerDef::FeatureGeom*> collectColocatedEventFeatures(
    const LayerDef& layer,
    const LayerDef::FeatureGeom& anchor) {
    std::vector<const LayerDef::FeatureGeom*> matches;
    matches.reserve(8);
    for (const auto& fg : layer.features) {
        if (samePointLocation(fg, anchor)) matches.push_back(&fg);
    }
    return matches;
}

void drawEventAvatar(const LayerDef::FeatureGeom& fg, float max_w, float max_h) {
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

void drawEventListEntry(const LayerDef::FeatureGeom& fg, const char* row_id, bool clickable) {
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

void drawPointLayerHeader(const LayerDef& layer, const LayerDef::FeatureGeom* fg = nullptr) {
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

void drawFeatureProperties(const char* title, const LayerDef::FeatureGeom& fg) {
    ImGui::TextUnformatted(title);
    for (const auto& kv : fg.properties) {
        std::string v = trimDisplayValue(kv.second);
        if (v.empty()) continue;
        ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
    }
}

void drawRealPropertySummary(const LayerDef::FeatureGeom* rp) {
    if (!rp) {
        ImGui::TextDisabled("No matching real-property record.");
        return;
    }
    auto text_prop = [&](const char* label, const std::string& value) {
        if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
    };
    text_prop("Address", firstDisplayProperty(*rp, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"}));
    text_prop("Owner", firstDisplayProperty(*rp, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"}));
    text_prop("Use", firstDisplayProperty(*rp, {"LU", "LANDUSE", "USE_CODE", "USE"}));
    text_prop("Tax Base", firstDisplayProperty(*rp, {"TAXBASE", "ARTAXBAS"}));
    text_prop("Current Land", firstDisplayProperty(*rp, {"CURRLAND"}));
    text_prop("Current Improvements", firstDisplayProperty(*rp, {"CURRIMPR"}));
    text_prop("Sale Price", firstDisplayProperty(*rp, {"SALEPRIC"}));
    text_prop("Sale Date", firstDisplayProperty(*rp, {"SALEDATE"}));
    std::string deed_book = firstDisplayProperty(*rp, {"DEEDBOOK"});
    std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
    text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
    text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
    ImGui::TextDisabled("Source: Local property records when available");
}

std::string pointFeatureTitle(const LayerDef::FeatureGeom& fg) {
    return firstDisplayProperty(
        fg,
        {"name", "Name", "NAME", "facility_name", "facility_n", "school_name", "school_nam",
         "market_name", "market_nam", "settlement_name", "settlement", "church_name",
         "industry_name", "station_name", "waterpoint_name"});
}

std::string eventFeatureOpenUrl(const LayerDef::FeatureGeom& fg) {
    std::string url = firstDisplayProperty(fg, {"url", "URL"});
    if (!url.empty()) return url;
    return firstDisplayProperty(fg, {"source_url", "source", "Source"});
}

std::string pointFeatureOpenUrl(const LayerDef& layer, const LayerDef::FeatureGeom& fg) {
    if (!isLikelyEventPointLayer(layer)) return {};
    return eventFeatureOpenUrl(fg);
}

void drawEventPointSummary(const LayerDef& layer, const LayerDef::FeatureGeom& fg) {
    const std::vector<const LayerDef::FeatureGeom*> colocated = collectColocatedEventFeatures(layer, fg);
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

void drawPointFeatureSummary(const LayerDef& layer, const LayerDef::FeatureGeom& fg) {
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

void handleMapInspection(const MapInspectionContext& ctx) {
    static bool event_stack_popup_open = false;
    static int event_stack_layer_idx = -1;
    static float event_stack_lon = 0.0f;
    static float event_stack_lat = 0.0f;

    if (!ctx.hover_state || !ctx.layers || !ctx.parcel_selection) return;
    const LayerDef::FeatureGeom* hovered_parcel = ctx.hover_state->hovered_parcel;
    const size_t hovered_parcel_idx = ctx.hover_state->hovered_parcel_idx;
    const LayerDef::FeatureGeom* hovered_zone = ctx.hover_state->hovered_zone;
    const size_t hovered_zone_idx = ctx.hover_state->hovered_zone_idx;
    const LayerDef::FeatureGeom* hovered_point = ctx.hover_state->hovered_point;
    const int hovered_point_layer_idx = ctx.hover_state->hovered_point_layer_idx;

    const bool click_select =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= 36.0f;

    if (ctx.map_hovered && click_select && hovered_point && hovered_point_layer_idx >= 0 &&
        (size_t)hovered_point_layer_idx < ctx.layers->size()) {
        const LayerDef& hovered_point_layer = (*ctx.layers)[(size_t)hovered_point_layer_idx];
        if (isLikelyEventPointLayer(hovered_point_layer)) {
            const std::vector<const LayerDef::FeatureGeom*> colocated =
                collectColocatedEventFeatures(hovered_point_layer, *hovered_point);
            if (colocated.size() > 1) {
                event_stack_popup_open = true;
                event_stack_layer_idx = hovered_point_layer_idx;
                event_stack_lon = hovered_point->extent.min_lon;
                event_stack_lat = hovered_point->extent.min_lat;
                ImGui::OpenPopup("Event Location List");
                hovered_parcel = nullptr;
                hovered_zone = nullptr;
            } else {
                const std::string open_url = pointFeatureOpenUrl(hovered_point_layer, *hovered_point);
                if (!open_url.empty()) {
                    openUrlInBrowser(open_url);
                    return;
                }
            }
        } else {
            const std::string open_url = pointFeatureOpenUrl(hovered_point_layer, *hovered_point);
            if (!open_url.empty()) {
                openUrlInBrowser(open_url);
                return;
            }
        }
    }

    if (ctx.map_hovered && ctx.parcel_inspect_active && click_select && hovered_parcel != nullptr) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
            if (selectParcel(*ctx.parcel_selection, hovered_parcel_idx, (*ctx.layers)[(size_t)ctx.parcel_layer_idx].features.size(), ctrl) &&
                ctx.open_parcel_element) {
                ctx.open_parcel_element(hovered_parcel_idx);
            }
        }
        if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = false;
        if (ctx.selected_zone_idx) *ctx.selected_zone_idx = (size_t)-1;
    } else if (ctx.map_hovered && ctx.zoning_inspect_active && click_select && hovered_zone != nullptr) {
        if (ctx.show_selected_zone_details) *ctx.show_selected_zone_details = true;
        if (ctx.selected_zone_idx) *ctx.selected_zone_idx = hovered_zone_idx;
        clearParcelSelection(*ctx.parcel_selection);
    }

    if (ctx.parcel_hover_active && ctx.map_hovered && hovered_parcel) {
        std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
        const auto& vac_notice_vec = ctx.parcel_vac_notice_by_feature ? *ctx.parcel_vac_notice_by_feature : std::vector<int>{};
        const auto& vac_rehab_vec = ctx.parcel_vac_rehab_by_feature ? *ctx.parcel_vac_rehab_by_feature : std::vector<int>{};
        const auto& tax_lien_vec = ctx.parcel_tax_lien_by_feature ? *ctx.parcel_tax_lien_by_feature : std::vector<int>{};
        const auto& tax_sale_vec = ctx.parcel_tax_sale_by_feature ? *ctx.parcel_tax_sale_by_feature : std::vector<int>{};
        const auto& tax_lien_amount_vec = ctx.parcel_tax_lien_amount_by_feature ? *ctx.parcel_tax_lien_amount_by_feature : std::vector<double>{};
        const auto& tax_sale_amount_vec = ctx.parcel_tax_sale_amount_by_feature ? *ctx.parcel_tax_sale_amount_by_feature : std::vector<double>{};
        int vac_notice = (hovered_parcel_idx < vac_notice_vec.size()) ? vac_notice_vec[hovered_parcel_idx] : 0;
        int vac_rehab = (hovered_parcel_idx < vac_rehab_vec.size()) ? vac_rehab_vec[hovered_parcel_idx] : 0;
        int tax_lien = (hovered_parcel_idx < tax_lien_vec.size()) ? tax_lien_vec[hovered_parcel_idx] : 0;
        int tax_sale = (hovered_parcel_idx < tax_sale_vec.size()) ? tax_sale_vec[hovered_parcel_idx] : 0;
        double tax_lien_amount = (hovered_parcel_idx < tax_lien_amount_vec.size()) ? tax_lien_amount_vec[hovered_parcel_idx] : 0.0;
        double tax_sale_amount = (hovered_parcel_idx < tax_sale_amount_vec.size()) ? tax_sale_amount_vec[hovered_parcel_idx] : 0.0;
        const LayerDef::FeatureGeom* hovered_zoning = hovered_zone;
        if (ctx.zoning_layer_idx >= 0 && (size_t)ctx.zoning_layer_idx < ctx.layers->size()) {
            const float qlon = (hovered_parcel->extent.min_lon + hovered_parcel->extent.max_lon) * 0.5f;
            const float qlat = (hovered_parcel->extent.min_lat + hovered_parcel->extent.max_lat) * 0.5f;
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

        const LayerDef::FeatureGeom* hovered_rp = ctx.real_property_for_parcel ? ctx.real_property_for_parcel(*hovered_parcel) : nullptr;
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
    }
    if (ctx.zoning_hover_active && ctx.map_hovered && !(ctx.parcel_hover_active && hovered_parcel) && hovered_zone && ctx.zoning_metadata) {
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
                LayerDef::FeatureGeom anchor;
                anchor.extent.min_lon = event_stack_lon;
                anchor.extent.min_lat = event_stack_lat;
                const std::vector<const LayerDef::FeatureGeom*> colocated = collectColocatedEventFeatures(layer, anchor);
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
