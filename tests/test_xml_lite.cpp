#include <cassert>
#include <cstdio>
#include <string>

#include "core/xml_lite.h"

using swiftdm::XmlNode;

static void test_basic_parse() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml(R"(<?xml version="1.0"?><root attr="val"><child>text</child></root>)", &root, &err);
    assert(ok);
    assert(root.name == "root");
    assert(root.attribute("attr") == "val");
    const XmlNode* child = root.child("child");
    assert(child);
    assert(child->text == "text");
    std::printf("  basic_parse OK\n");
}

static void test_self_closing() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml("<r><a x=\"1\"/><b/></r>", &root, &err);
    assert(ok);
    assert(root.name == "r");
    const XmlNode* a = root.child("a");
    assert(a && a->attribute("x") == "1");
    assert(root.child("b"));
    std::printf("  self_closing OK\n");
}

static void test_children_named() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml("<r><c id=\"1\"/><c id=\"2\"/><d/></r>", &root, &err);
    assert(ok);
    auto cs = root.children_named("c");
    assert(cs.size() == 2);
    assert(cs[0]->attribute("id") == "1");
    assert(cs[1]->attribute("id") == "2");
    std::printf("  children_named OK\n");
}

static void test_bad_xml() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml("not xml at all", &root, &err);
    assert(!ok);
    assert(!err.empty());
    std::printf("  bad_xml OK\n");
}

static void test_nested() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml("<a><b><c val=\"deep\"/></b></a>", &root, &err);
    assert(ok);
    const XmlNode* b = root.child("b");
    assert(b);
    const XmlNode* c = b->child("c");
    assert(c && c->attribute("val") == "deep");
    std::printf("  nested OK\n");
}

static void test_cdata() {
    XmlNode root;
    std::string err;
    bool ok = swiftdm::parse_xml("<r><![CDATA[hello <world>]]></r>", &root, &err);
    assert(ok);
    assert(root.text == "hello <world>");
    std::printf("  cdata OK\n");
}

int main() {
    std::printf("xml_lite tests:\n");
    test_basic_parse();
    test_self_closing();
    test_children_named();
    test_bad_xml();
    test_nested();
    test_cdata();
    std::printf("all xml_lite tests passed\n");
    return 0;
}
