#include "engine/dash.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/xml_lite.h"
#include "engine/hls.h"

namespace swiftdm {
namespace {

std::string lower_copy(const std::string& text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string strip_quotes(const std::string& text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

int64_t parse_int64(const std::string& text) {
    if (text.empty()) return 0;
    try {
        return std::stoll(text);
    } catch (...) {
        return 0;
    }
}

// Concatenates base URL and relative path.
std::string resolve_mpd_url(const std::string& base, const std::string& path) {
    if (path.empty()) return base;
    if (path.find("://") != std::string::npos || path.substr(0, 2) == "//") {
        return path;
    }
    if (base.empty()) return path;
    if (path.front() == '/') {
        const size_t scheme_end = base.find("://");
        if (scheme_end == std::string::npos) return path;
        const size_t path_start = base.find('/', scheme_end + 3);
        return base.substr(0, path_start) + path;
    }
    return directory_of(base) + path;
}

// Collects BaseURL text at this level plus inherited ones.
std::vector<std::string> collect_base_urls(const XmlNode& node,
                                           const std::vector<std::string>& inherited) {
    std::vector<std::string> result = inherited;
    for (const XmlNode* child : node.children_named("BaseURL")) {
        if (!child->text.empty()) result.push_back(child->text);
    }
    return result;
}

struct TemplateSet {
    std::string initialization;
    std::string media;
    int64_t timescale = 1;
    int64_t start_number = 1;
    std::vector<std::pair<int64_t, int64_t>> timeline;  // (time, duration)
};

TemplateSet parse_segment_template(const XmlNode& node) {
    TemplateSet out;
    out.initialization = node.attribute("initialization");
    out.media = node.attribute("media");
    out.timescale = std::max<int64_t>(1, parse_int64(node.attribute("timescale")));
    out.start_number = std::max<int64_t>(1, parse_int64(node.attribute("startNumber")));

    const XmlNode* timeline_node = node.child("SegmentTimeline");
    if (timeline_node) {
        int64_t last_time = 0;
        for (const XmlNode* seg : timeline_node->children_named("S")) {
            int64_t t = parse_int64(seg->attribute("t"));
            int64_t d = parse_int64(seg->attribute("d"));
            if (d <= 0) continue;
            int64_t r = parse_int64(seg->attribute("r"));
            if (t > 0) last_time = t;
            for (int64_t i = 0; i <= r; ++i) {
                out.timeline.emplace_back(last_time, d);
                last_time += d;
            }
        }
    }
    return out;
}

ui::MediaVariant build_representation(const XmlNode& rep_node,
                                      const std::string& content_type,
                                      const std::vector<std::string>& base_urls,
                                      const std::string& base_url,
                                      const std::string& manifest_url,
                                      const TemplateSet* inherited_template,
                                      const XmlNode* segment_list) {
    ui::MediaVariant variant;
    variant.kind = "dash";
    variant.bandwidth = parse_int64(rep_node.attribute("bandwidth"));
    variant.width = static_cast<int>(parse_int64(rep_node.attribute("width")));
    variant.height = static_cast<int>(parse_int64(rep_node.attribute("height")));
    variant.audio_only = lower_copy(content_type) == "audio";
    variant.codecs = strip_quotes(rep_node.attribute("codecs"));

    std::string id = rep_node.attribute("id");
    if (id.empty()) id = "0";
    variant.representation_id = id;
    variant.manifest_url = manifest_url;

    std::vector<std::string> rep_bases = collect_base_urls(rep_node, base_urls);
    const std::string my_base = rep_bases.empty() ? base_url : rep_bases.back();

    const XmlNode* segment_template = rep_node.child("SegmentTemplate");
    const XmlNode* seg_base = rep_node.child("SegmentBase");

    const std::string label = rep_node.attribute("label");
    variant.label = label.empty() ? media_variant_label(variant) : label;

    // A representation may inherit the AdaptationSet's SegmentTemplate; the
    // template is the common case, so it is resolved before the alternatives.
    TemplateSet own_template;
    const TemplateSet* templ = inherited_template;
    if (segment_template) {
        own_template = parse_segment_template(*segment_template);
        if (own_template.media.empty() && inherited_template) {
            own_template.media = inherited_template->media;
            own_template.initialization = inherited_template->initialization;
            own_template.timescale = inherited_template->timescale;
            own_template.start_number = inherited_template->start_number;
            own_template.timeline = inherited_template->timeline;
        }
        templ = &own_template;
    }
    if (templ) {
        if (!templ->initialization.empty()) {
            variant.init_url = resolve_mpd_url(
                my_base, resolve_segment_template(templ->initialization, id,
                                                  variant.bandwidth,
                                                  templ->start_number, 0));
        }
        if (!templ->timeline.empty()) {
            int64_t total_time = 0;
            for (const auto& entry : templ->timeline) total_time += entry.second;
            variant.size_estimate = static_cast<uint64_t>(
                total_time * variant.bandwidth / (8 * templ->timescale));
            for (size_t i = 0; i < templ->timeline.size(); ++i) {
                variant.segment_urls.push_back(resolve_mpd_url(
                    my_base,
                    resolve_segment_template(templ->media, id, variant.bandwidth,
                                             templ->start_number + static_cast<int64_t>(i),
                                             templ->timeline[i].first)));
            }
        }
    } else if (segment_list) {
        const XmlNode* init = segment_list->child("Initialization");
        if (init && init->has_attribute("sourceURL")) {
            variant.init_url = resolve_mpd_url(my_base, init->attribute("sourceURL"));
        }
        for (const XmlNode* seg : segment_list->children_named("SegmentURL")) {
            if (seg->has_attribute("media")) {
                variant.segment_urls.push_back(
                    resolve_mpd_url(my_base, seg->attribute("media")));
            }
        }
    } else if (seg_base) {
        // Single-file representation: the whole file is one segment.
        variant.segment_urls.push_back(my_base);
    }

    // The engine downloads the MPD and selects this representation by id; the
    // variant URL is always the manifest URL.
    variant.url = manifest_url;
    if (variant.url.empty()) variant.url = my_base;
    return variant;
}

}  // namespace

bool is_dash_url(const std::string& url, const std::string& mime_type) {
    const std::string lowered = lower_copy(mime_type);
    if (lowered.find("dash+xml") != std::string::npos) return true;
    if (lowered.find("mpd") != std::string::npos) return true;
    const std::string path = lower_copy(url);
    return path.size() > 4 && path.substr(path.size() - 4) == ".mpd";
}

std::string resolve_segment_template(const std::string& templ,
                                     const std::string& representation_id,
                                     int64_t bandwidth, int64_t n, int64_t t) {
    std::string out;
    out.reserve(templ.size() * 2);
    size_t i = 0;
    while (i < templ.size()) {
        if (templ[i] == '$') {
            const size_t end = templ.find('$', i + 1);
            if (end == std::string::npos) {
                out += templ[i++];
                continue;
            }
            const std::string key = templ.substr(i + 1, end - i - 1);
            if (key == "RepresentationID") {
                out += representation_id;
            } else if (key == "Bandwidth") {
                out += std::to_string(bandwidth);
            } else if (key == "Number") {
                out += std::to_string(n);
            } else if (key == "Time") {
                out += std::to_string(t);
            } else if (key.empty()) {
                out += '$';  // $$
            } else {
                out += "$" + key + "$";
            }
            i = end + 1;
        } else {
            out.push_back(templ[i++]);
        }
    }
    return out;
}

bool parse_dash_manifest(const std::string& text, const std::string& base_url,
                         DashManifest* out, std::string* error) {
    if (!out) {
        if (error) *error = "no DASH output buffer";
        return false;
    }
    *out = DashManifest{};

    XmlNode root;
    if (!parse_xml(text, &root, error)) return false;
    if (root.name != "MPD") {
        if (error) *error = "root element is not <MPD>";
        return false;
    }
    if (root.attribute("type") == "dynamic") {
        out->dynamic = true;
        if (error) *error = "live/dynamic DASH streams are not supported";
        return false;
    }
    out->media_presentation_duration = root.attribute("mediaPresentationDuration");

    const XmlNode* period = root.child("Period");
    if (!period) {
        if (error) *error = "<Period> not found";
        return false;
    }

    const std::vector<std::string> mpd_bases = collect_base_urls(root, {});
    const std::vector<std::string> period_bases = collect_base_urls(*period, mpd_bases);

    for (const XmlNode* adapt : period->children_named("AdaptationSet")) {
        const std::string mime = lower_copy(adapt->attribute("mimeType"));
        std::string content_type = adapt->attribute("contentType");
        if (content_type.empty() && !mime.empty()) {
            const size_t slash = mime.find('/');
            if (slash != std::string::npos) content_type = mime.substr(0, slash);
        }
        if (content_type != "video" && content_type != "audio") continue;

        const std::vector<std::string> adapt_bases = collect_base_urls(*adapt, period_bases);
        const XmlNode* templ_node = adapt->child("SegmentTemplate");
        TemplateSet adapt_template;
        if (templ_node) adapt_template = parse_segment_template(*templ_node);
        const XmlNode* segment_list = adapt->child("SegmentList");

        for (const XmlNode* rep : adapt->children_named("Representation")) {
            ui::MediaVariant variant = build_representation(
                *rep, content_type, adapt_bases, base_url, base_url,
                templ_node ? &adapt_template : nullptr, segment_list);
            if (!variant.is_valid()) continue;
            if (content_type == "video") {
                out->video_representations.push_back(std::move(variant));
            } else {
                out->audio_representations.push_back(std::move(variant));
            }
        }
    }

    if (out->video_representations.empty() && out->audio_representations.empty()) {
        if (error) *error = "no usable video or audio representation found";
        return false;
    }
    sort_media_variants(out->video_representations);
    sort_media_variants(out->audio_representations);
    return true;
}

}  // namespace swiftdm
