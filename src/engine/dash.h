#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/media_variant.h"

namespace swiftdm {

// Parsed DASH manifest subset (docs/spec/08-media-download.md §9).
//
// Supported: one Period, one AdaptationSet per content type (video + audio),
// SegmentTemplate with $Number$/$Time$/$RepresentationID$/$Bandwidth$ and
// SegmentTimeline, SegmentList with SegmentURL@media, SegmentBase with sidx.
// Unsupported multi-period, live/dynamic, or DRM ContentProtection: the parser
// rejects them instead of producing broken output.
struct DashManifest {
    bool dynamic = false;
    std::string media_presentation_duration;  // raw attribute, for info text
    std::vector<ui::MediaVariant> video_representations;
    std::vector<ui::MediaVariant> audio_representations;
    // Base URLs inherited from MPD / Period / AdaptationSet / Representation.
    std::vector<std::string> base_urls;
    // Optional sidx for SegmentBase addressing.
    std::vector<std::pair<uint64_t, uint64_t>> sidx_references;  // (offset,size)
    uint64_t sidx_offset = 0;
    bool has_segment_base = false;
};

// Parses `text` (the MPD XML body). Returns false with a reason on failure.
bool parse_dash_manifest(const std::string& text, const std::string& base_url,
                         DashManifest* out, std::string* error);

// Resolves a DASH segment template, returning a concrete URL for segment `n`
// at presentation time `t`.
std::string resolve_segment_template(const std::string& templ,
                                     const std::string& representation_id,
                                     int64_t bandwidth, int64_t n, int64_t t);

// True when the MIME type is `application/dash+xml` or the URL path ends in
// `.mpd`.
bool is_dash_url(const std::string& url, const std::string& mime_type);

}  // namespace swiftdm
