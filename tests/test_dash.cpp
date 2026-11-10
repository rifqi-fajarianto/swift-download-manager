#include <cassert>
#include <cstdio>
#include <string>

#include "engine/dash.h"
#include "ui/media_variant.h"

using namespace swiftdm;

static const char* kSimpleMpd = R"(<?xml version="1.0"?>
<MPD type="static" mediaPresentationDuration="PT1M">
  <Period>
    <AdaptationSet contentType="video" mimeType="video/mp4">
      <Representation id="v1" bandwidth="5000000" width="1920" height="1080" codecs="avc1.640028">
        <BaseURL>https://cdn.example.com/v1/</BaseURL>
        <SegmentBase indexRange="0-999"/>
      </Representation>
      <Representation id="v2" bandwidth="2500000" width="1280" height="720" codecs="avc1.4d401f">
        <BaseURL>https://cdn.example.com/v2/</BaseURL>
        <SegmentBase indexRange="0-999"/>
      </Representation>
    </AdaptationSet>
    <AdaptationSet contentType="audio" mimeType="audio/mp4">
      <Representation id="a1" bandwidth="128000" codecs="mp4a.40.2">
        <BaseURL>https://cdn.example.com/a1.mp4</BaseURL>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>)";

static void test_simple_mpd() {
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(kSimpleMpd, "https://cdn.example.com/manifest.mpd", &m, &err);
    assert(ok);
    assert(!m.dynamic);
    assert(m.video_representations.size() == 2);
    assert(m.audio_representations.size() == 1);
    // Best-first sorting: 1080p before 720p.
    assert(m.video_representations[0].height == 1080);
    assert(m.video_representations[1].height == 720);
    assert(m.audio_representations[0].audio_only);
    // DASH variants point at the manifest; the engine selects the rendition
    // by representation id.
    assert(m.video_representations[0].representation_id == "v1");
    assert(m.video_representations[0].manifest_url ==
           "https://cdn.example.com/manifest.mpd");
    assert(m.video_representations[0].url ==
           "https://cdn.example.com/manifest.mpd");
    // SegmentBase representation: the whole file is one segment.
    assert(m.video_representations[0].segment_urls.size() == 1);
    std::printf("  simple_mpd OK\n");
}

static const char* kTemplateMpd = R"(<?xml version="1.0"?>
<MPD type="static" mediaPresentationDuration="PT2M">
  <Period>
    <AdaptationSet contentType="video" mimeType="video/mp4">
      <SegmentTemplate timescale="90000" media="seg-$RepresentationID$-$Number$.m4s"
                       initialization="init-$RepresentationID$.m4s" startNumber="1">
        <SegmentTimeline>
          <S t="0" d="180000" r="5"/>
        </SegmentTimeline>
      </SegmentTemplate>
      <Representation id="hd" bandwidth="4000000" width="1920" height="1080"/>
      <Representation id="sd" bandwidth="1500000" width="854" height="480"/>
    </AdaptationSet>
  </Period>
</MPD>)";

static void test_template_mpd() {
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(kTemplateMpd, "https://cdn.example.com/stream.mpd", &m, &err);
    assert(ok);
    assert(m.video_representations.size() == 2);
    assert(m.video_representations[0].height == 1080);
    assert(m.video_representations[0].url ==
           "https://cdn.example.com/stream.mpd");
    assert(m.video_representations[0].size_estimate > 0);
    assert(m.video_representations[0].segment_urls.size() == 6);
    std::printf("  template_mpd OK\n");
}

static void test_dynamic_rejected() {
    const char* mpd = R"(<?xml version="1.0"?><MPD type="dynamic"><Period/></MPD>)";
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(mpd, "", &m, &err);
    assert(!ok);
    assert(err.find("dynamic") != std::string::npos);
    std::printf("  dynamic_rejected OK\n");
}

static void test_resolve_template() {
    std::string result = resolve_segment_template(
        "chunk-$RepresentationID$-$Number$-$Bandwidth$-$Time$.m4s",
        "v1", 5000000, 42, 900000);
    assert(result == "chunk-v1-42-5000000-900000.m4s");
    std::printf("  resolve_template OK\n");
}

static void test_is_dash_url() {
    assert(is_dash_url("https://x.com/stream.mpd", ""));
    assert(is_dash_url("https://x.com/api", "application/dash+xml"));
    assert(!is_dash_url("https://x.com/video.mp4", "video/mp4"));
    std::printf("  is_dash_url OK\n");
}

static void test_no_representations() {
    const char* mpd = R"(<?xml version="1.0"?>
<MPD type="static"><Period>
  <AdaptationSet contentType="text"><Representation id="s1"/></AdaptationSet>
</Period></MPD>)";
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(mpd, "", &m, &err);
    assert(!ok);
    assert(err.find("no usable") != std::string::npos);
    std::printf("  no_representations OK\n");
}

static void test_segment_list() {
    const char* mpd = R"(<?xml version="1.0"?>
<MPD type="static"><Period>
  <AdaptationSet contentType="audio" mimeType="audio/mp4">
    <SegmentList>
      <Initialization sourceURL="init.mp4"/>
      <SegmentURL media="seg1.mp4"/>
      <SegmentURL media="seg2.mp4"/>
    </SegmentList>
    <Representation id="a1" bandwidth="64000"/>
  </AdaptationSet>
</Period></MPD>)";
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(mpd, "https://cdn.example.com/manifest.mpd", &m, &err);
    assert(ok);
    assert(m.audio_representations.size() == 1);
    assert(m.audio_representations[0].url ==
           "https://cdn.example.com/manifest.mpd");
    assert(m.audio_representations[0].segment_urls.size() == 2);
    std::printf("  segment_list OK\n");
}

static void test_inherited_template() {
    // The SegmentTemplate can live on the AdaptationSet; the representation
    // inherits it and the initialization template must still substitute
    // $RepresentationID$.
    const char* mpd = R"(<?xml version="1.0"?>
<MPD type="static"><Period>
  <AdaptationSet contentType="video" mimeType="video/mp4">
    <SegmentTemplate timescale="90000" startNumber="1"
                     initialization="init-$RepresentationID$.mp4"
                     media="seg-$RepresentationID$-$Number$.m4s">
      <SegmentTimeline><S t="0" d="180000" r="1"/></SegmentTimeline>
    </SegmentTemplate>
    <Representation id="v1" bandwidth="5000000" width="1920" height="1080"/>
  </AdaptationSet>
</Period></MPD>)";
    DashManifest m;
    std::string err;
    bool ok = parse_dash_manifest(mpd, "https://cdn.example.com/manifest.mpd", &m, &err);
    assert(ok);
    assert(m.video_representations.size() == 1);
    assert(m.video_representations[0].init_url ==
           "https://cdn.example.com/init-v1.mp4");
    assert(m.video_representations[0].segment_urls.size() == 2);
    assert(m.video_representations[0].segment_urls[0] ==
           "https://cdn.example.com/seg-v1-1.m4s");
    std::printf("  inherited_template OK\n");
}

int main() {
    std::printf("dash tests:\n");
    test_simple_mpd();
    test_template_mpd();
    test_dynamic_rejected();
    test_resolve_template();
    test_is_dash_url();
    test_no_representations();
    test_segment_list();
    test_inherited_template();
    std::printf("all dash tests passed\n");
    return 0;
}
