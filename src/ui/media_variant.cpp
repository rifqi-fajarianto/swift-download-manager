#include "ui/media_variant.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace swiftdm::ui {

std::string media_variant_label(const MediaVariant& variant) {
    std::string label;
    if (variant.height > 0) {
        label += std::to_string(variant.height) + "p";
    } else if (variant.audio_only) {
        label += "audio";
    }
    if (variant.bandwidth > 0) {
        if (!label.empty()) label += " · ";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f Mb/s", variant.bandwidth / 1'000'000.0);
        label += buf;
    }
    if (!variant.codecs.empty()) {
        if (!label.empty()) label += " · ";
        label += variant.codecs;
    }
    if (label.empty()) {
        label = variant.kind.empty() ? "default" : variant.kind;
    }
    return label;
}

void sort_media_variants(std::vector<MediaVariant>& variants) {
    std::stable_sort(variants.begin(), variants.end(),
                     [](const MediaVariant& a, const MediaVariant& b) {
                         if (a.audio_only != b.audio_only) return !a.audio_only;
                         const int area_a = a.width * a.height;
                         const int area_b = b.width * b.height;
                         if (area_a != area_b) return area_a > area_b;
                         return a.bandwidth > b.bandwidth;
                     });
}

}  // namespace swiftdm::ui
