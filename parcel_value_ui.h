#pragma once

#include "app_utils.h"
#include "imgui.h"
#include "parcel_unified.h"

inline void drawParcelValueUnavailableReason(const UnifiedParcelRecord* rec) {
    if (!rec) {
        ImGui::TextDisabled("Reason: no unified parcel record is available for this parcel.");
    } else if (!rec->has_property_record) {
        ImGui::TextDisabled("Reason: no joined property record was found for this parcel.");
    } else {
        ImGui::TextDisabled("Reason: the joined property record has no usable current value, tax base, land/improvement value, or sale price.");
    }
}

inline void drawParcelCurrentValueTotal(double value, const UnifiedParcelRecord* active_rec) {
    if (value > 0.0) {
        ImGui::Text("Current Parcel Value (Total): %s", formatUsd(value, 2).c_str());
    } else {
        ImGui::TextDisabled("Current Parcel Value (Total): unavailable");
        drawParcelValueUnavailableReason(active_rec);
    }
}

inline void drawParcelCurrentValueDetail(const UnifiedParcelRecord& rec) {
    if (rec.current_value > 0.0) {
        ImGui::TextWrapped("Current Value: %s", formatUsd(rec.current_value, 2).c_str());
    } else {
        ImGui::TextDisabled("Current Value: unavailable");
        drawParcelValueUnavailableReason(&rec);
    }
}
