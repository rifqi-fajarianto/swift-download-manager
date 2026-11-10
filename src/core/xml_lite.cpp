#include "core/xml_lite.h"

#include <cctype>
#include <cstdint>
#include <string>

#include "core/util.h"

namespace swiftdm {
namespace {

// Decodes the five predefined entities plus numeric character references.
// Anything else fails, because a manifest that uses an undeclared entity is not
// something this parser can safely interpret.
bool decode_entities(const std::string& text, std::string* out, std::string* error) {
    out->clear();
    out->reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out->push_back(text[i]);
            continue;
        }
        const size_t end = text.find(';', i + 1);
        if (end == std::string::npos || end - i > 12) {
            if (error) *error = "unterminated XML entity";
            return false;
        }
        const std::string name = text.substr(i + 1, end - i - 1);
        if (name == "amp") {
            out->push_back('&');
        } else if (name == "lt") {
            out->push_back('<');
        } else if (name == "gt") {
            out->push_back('>');
        } else if (name == "quot") {
            out->push_back('"');
        } else if (name == "apos") {
            out->push_back('\'');
        } else if (!name.empty() && name[0] == '#') {
            // Numeric reference: decimal (#38) or hex (#x26).
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const std::string digits = name.substr(hex ? 2 : 1);
            if (digits.empty()) {
                if (error) *error = "empty numeric XML entity";
                return false;
            }
            uint32_t code = 0;
            for (const char digit : digits) {
                uint32_t value = 0;
                if (digit >= '0' && digit <= '9') {
                    value = static_cast<uint32_t>(digit - '0');
                } else if (hex && digit >= 'a' && digit <= 'f') {
                    value = static_cast<uint32_t>(digit - 'a' + 10);
                } else if (hex && digit >= 'A' && digit <= 'F') {
                    value = static_cast<uint32_t>(digit - 'A' + 10);
                } else {
                    if (error) *error = "invalid numeric XML entity";
                    return false;
                }
                code = code * (hex ? 16 : 10) + value;
                if (code > 0x10FFFF) {
                    if (error) *error = "numeric XML entity out of range";
                    return false;
                }
            }
            // Encode as UTF-8. Surrogates and 0 are refused: a manifest has no
            // business carrying them.
            if (code == 0 || (code >= 0xD800 && code <= 0xDFFF)) {
                if (error) *error = "invalid XML character reference";
                return false;
            }
            if (code < 0x80) {
                out->push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code < 0x10000) {
                out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out->push_back(static_cast<char>(0xF0 | (code >> 18)));
                out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        } else {
            if (error) *error = "unknown XML entity '&" + name + ";'";
            return false;
        }
        i = end;
    }
    return true;
}

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '-' || c == '_' ||
           c == '.';
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Parses `name="value"` pairs until the closing `>` or `/>`.
bool parse_attributes(const std::string& text, size_t* pos, XmlNode* node, bool* self_closing,
                      std::string* error) {
    *self_closing = false;
    while (*pos < text.size()) {
        while (*pos < text.size() && is_space(text[*pos])) ++*pos;
        if (*pos >= text.size()) break;
        if (text[*pos] == '/') {
            if (*pos + 1 < text.size() && text[*pos + 1] == '>') {
                *self_closing = true;
                *pos += 2;
                return true;
            }
            if (error) *error = "stray '/' in a start tag";
            return false;
        }
        if (text[*pos] == '>') {
            ++*pos;
            return true;
        }
        const size_t name_start = *pos;
        while (*pos < text.size() && is_name_char(text[*pos])) ++*pos;
        if (*pos == name_start) {
            if (error) *error = "malformed attribute name";
            return false;
        }
        const std::string name = text.substr(name_start, *pos - name_start);
        while (*pos < text.size() && is_space(text[*pos])) ++*pos;
        if (*pos >= text.size() || text[*pos] != '=') {
            if (error) *error = "attribute without a value";
            return false;
        }
        ++*pos;
        while (*pos < text.size() && is_space(text[*pos])) ++*pos;
        if (*pos >= text.size() || (text[*pos] != '"' && text[*pos] != '\'')) {
            if (error) *error = "unquoted attribute value";
            return false;
        }
        const char quote = text[*pos];
        ++*pos;
        const size_t value_start = *pos;
        while (*pos < text.size() && text[*pos] != quote) ++*pos;
        if (*pos >= text.size()) {
            if (error) *error = "unterminated attribute value";
            return false;
        }
        std::string raw = text.substr(value_start, *pos - value_start);
        std::string value;
        if (!decode_entities(raw, &value, error)) return false;
        ++*pos;
        node->attributes.emplace_back(name, value);
    }
    if (error) *error = "unterminated start tag";
    return false;
}

}  // namespace

std::string xml_local_name(const std::string& name) {
    const size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::string XmlNode::attribute(const std::string& name) const {
    for (const auto& [key, value] : attributes) {
        if (xml_local_name(key) == name) return value;
    }
    return {};
}

bool XmlNode::has_attribute(const std::string& name) const {
    for (const auto& [key, value] : attributes) {
        if (xml_local_name(key) == name) return true;
    }
    return false;
}

const XmlNode* XmlNode::child(const std::string& name) const {
    for (const XmlNode& node : children) {
        if (node.name == name) return &node;
    }
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::children_named(const std::string& name) const {
    std::vector<const XmlNode*> out;
    for (const XmlNode& node : children) {
        if (node.name == name) out.push_back(&node);
    }
    return out;
}

bool parse_xml(const std::string& text, XmlNode* root, std::string* error, int max_depth) {
    if (!root) {
        if (error) *error = "no XML output buffer";
        return false;
    }
    *root = XmlNode{};

    std::vector<XmlNode*> stack;
    std::vector<std::string> open_names;
    size_t pos = 0;
    bool seen_root = false;

    while (pos < text.size()) {
        const size_t open = text.find('<', pos);
        if (open == std::string::npos) {
            // Trailing text outside the document element is not allowed.
            if (stack.empty()) {
                const std::string tail = trim(text.substr(pos));
                if (!tail.empty()) {
                    if (error) *error = "text outside the document element";
                    return false;
                }
            }
            break;
        }
        // Character data before this tag belongs to the current element.
        if (open > pos && !stack.empty()) {
            std::string decoded;
            if (!decode_entities(text.substr(pos, open - pos), &decoded, error)) return false;
            stack.back()->text += decoded;
        }

        if (open + 1 >= text.size()) {
            if (error) *error = "truncated XML";
            return false;
        }
        const char next = text[open + 1];

        if (next == '?') {
            // XML declaration or processing instruction: skipped entirely.
            const size_t end = text.find("?>", open + 2);
            if (end == std::string::npos) {
                if (error) *error = "unterminated processing instruction";
                return false;
            }
            pos = end + 2;
            continue;
        }
        if (next == '!') {
            // Comment, CDATA or DOCTYPE.
            if (text.compare(open, 4, "<!--") == 0) {
                const size_t end = text.find("-->", open + 4);
                if (end == std::string::npos) {
                    if (error) *error = "unterminated XML comment";
                    return false;
                }
                pos = end + 3;
                continue;
            }
            if (text.compare(open, 9, "<![CDATA[") == 0) {
                const size_t end = text.find("]]>", open + 9);
                if (end == std::string::npos) {
                    if (error) *error = "unterminated CDATA section";
                    return false;
                }
                if (!stack.empty()) stack.back()->text += text.substr(open + 9, end - open - 9);
                pos = end + 3;
                continue;
            }
            // A DOCTYPE would be the entry point for entity tricks; refusing it
            // keeps the parser predictable.
            if (error) *error = "DOCTYPE is not supported";
            return false;
        }
        if (next == '/') {
            const size_t end = text.find('>', open + 2);
            if (end == std::string::npos) {
                if (error) *error = "unterminated end tag";
                return false;
            }
            const std::string name = trim(text.substr(open + 2, end - open - 2));
            if (open_names.empty() || open_names.back() != name) {
                if (error) {
                    *error = open_names.empty()
                                 ? "unexpected end tag </" + name + ">"
                                 : "end tag </" + name + "> does not match <" +
                                       open_names.back() + ">";
                }
                return false;
            }
            open_names.pop_back();
            stack.pop_back();
            pos = end + 1;
            continue;
        }

        // Start tag.
        const size_t name_start = open + 1;
        size_t name_end = name_start;
        while (name_end < text.size() && is_name_char(text[name_end])) ++name_end;
        if (name_end == name_start) {
            if (error) *error = "malformed start tag";
            return false;
        }
        const std::string raw_name = text.substr(name_start, name_end - name_start);

        XmlNode node;
        node.name = xml_local_name(raw_name);
        pos = name_end;
        bool self_closing = false;
        if (!parse_attributes(text, &pos, &node, &self_closing, error)) return false;

        if (stack.empty()) {
            if (seen_root) {
                if (error) *error = "more than one document element";
                return false;
            }
            seen_root = true;
            *root = std::move(node);
            if (!self_closing) {
                stack.push_back(root);
                open_names.push_back(raw_name);
            }
            continue;
        }
        if (static_cast<int>(stack.size()) >= max_depth) {
            if (error) *error = "XML document is nested too deeply";
            return false;
        }
        stack.back()->children.push_back(std::move(node));
        if (!self_closing) {
            stack.push_back(&stack.back()->children.back());
            open_names.push_back(raw_name);
        }
    }

    if (!seen_root) {
        if (error) *error = "no XML document element";
        return false;
    }
    if (!open_names.empty()) {
        if (error) *error = "unclosed element <" + open_names.back() + ">";
        return false;
    }
    root->text = trim(root->text);
    return true;
}

}  // namespace swiftdm
