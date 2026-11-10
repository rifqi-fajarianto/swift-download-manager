#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swiftdm::ui {

// One selectable rendition of a media handoff (docs/spec/08-media-download.md
// §8). A plain file has exactly one variant (itself); an HLS master playlist
// and a DASH MPD produce one entry per rendition.
struct MediaVariant {
    std::string label;       // "1080p · 5.0 Mb/s · avc1.640028", shown in the combo
    std::string url;         // the URL the engine should download for this variant
    std::string kind;        // "file" | "hls" | "dash" | "mse"
    int64_t bandwidth = 0;   // bits per second, 0 when unknown
    int width = 0;
    int height = 0;
    std::string codecs;
    bool audio_only = false;
    // Estimated size in bytes (0 = unknown). HLS/DASH only know it after the
    // segment list is resolved, so it is usually 0 in the confirmation window.
    uint64_t size_estimate = 0;
    // Parent manifest URL (HLS master playlist, DASH MPD). Used for info and
    // for re-probing when the engine needs the full manifest again.
    std::string manifest_url;
    // DASH representation id, so the engine can pick the exact rendition.
    std::string representation_id;
    // Resolved DASH initialisation and media segment URLs (the engine fills
    // these when it downloads; the UI ignores them).
    std::string init_url;
    std::vector<std::string> segment_urls;

    bool is_valid() const { return !url.empty(); }
};

// One recorded MSE segment (docs/spec/08-media-download.md §6). `role` is
// "init" for the initialisation segment and "media" for media segments.
struct MediaSegment {
    std::string url;
    std::string role;       // "init" | "media"
    int order = 0;
    std::string content_type;
};

// Short human label for a variant: resolution when known, then bandwidth, then
// the codec string. Never empty, so the combo always shows something.
std::string media_variant_label(const MediaVariant& variant);

// Sorts variants best-first: video before audio-only, then by resolution, then
// by bandwidth. Stable for equal keys so the manifest order survives.
void sort_media_variants(std::vector<MediaVariant>& variants);

}  // namespace swiftdm::ui
