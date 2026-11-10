#pragma once

#include <string>
#include <vector>

namespace swiftdm {

// Minimal XML reader for DASH manifests (docs/spec/08-media-download.md §9).
//
// It is deliberately not a general XML implementation: no DTD, no namespaces,
// no entity expansion beyond the five predefined ones and numeric character
// references. The input is a remote manifest, so the parser is written to fail
// closed instead of guessing:
//
//   * unknown entities are an error (never expanded to nothing),
//   * a stray `</x>` or a truncated document is an error,
//   * nesting is tracked with a depth counter, so malformed input cannot make
//     the caller read a child as a sibling.
struct XmlNode {
    std::string name;         // local name, namespace prefix stripped
    std::string text;         // concatenated direct text content (trimmed)
    std::vector<XmlNode> children;
    std::vector<std::pair<std::string, std::string>> attributes;

    // Attribute value by local name (prefix stripped). Empty when absent.
    std::string attribute(const std::string& name) const;
    bool has_attribute(const std::string& name) const;
    // First child with that local name, or nullptr.
    const XmlNode* child(const std::string& name) const;
    // All children with that local name.
    std::vector<const XmlNode*> children_named(const std::string& name) const;
};

// Parses `text` and returns the document element. Returns false with a reason
// when the document is malformed or deeper than `max_depth`.
bool parse_xml(const std::string& text, XmlNode* root, std::string* error,
               int max_depth = 64);

// Local name of a possibly prefixed tag ("cenc:pssh" -> "pssh").
std::string xml_local_name(const std::string& name);

}  // namespace swiftdm
