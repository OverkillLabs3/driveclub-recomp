// SPDX-License-Identifier: GPL-2.0-or-later
// XML that can be edited in place. The document is not rebuilt from a tree: every element knows where it sits in the
// text, and an edit replaces only that stretch of text, so comments, indentation and attribute order stay as written.
#pragma once

#include "textfile.hpp"

namespace dcmod {

struct XmlAttr {
    std::string name, value;  // value as written, without its quotes and with entities still in it
    size_t value_begin = 0, value_end = 0;
    size_t begin = 0, end = 0;  // the whole name="value" text
};

struct XmlNode {
    std::string name;
    std::vector<XmlAttr> attrs;
    size_t begin = 0;        // the '<' of the start tag
    size_t start_end = 0;    // just after the '>' that ends the start tag
    size_t end = 0;          // just after the element (its closing tag, or its start tag when it is self closing)
    size_t content_begin = 0, content_end = 0;
    bool self_closing = false;
    int parent = -1;
    std::vector<size_t> children;

    const XmlAttr* Attr(std::string_view n) const {
        for (const XmlAttr& a : attrs) {
            if (a.name == n) return &a;
        }
        return nullptr;
    }
};

class XmlDoc {
public:
    // Throws an Error naming the line of the first mistake. The text must use '\n' line breaks.
    static XmlDoc Parse(const std::string& text);

    const std::string& text() const { return text_; }
    const XmlNode& node(size_t i) const { return nodes_[i]; }

    // Elements matching a path such as `ViewportQuality/Configuration[@name='default']/General`: names separated by
    // '/', '*' for any name, [@attr='value'] or [@attr] to filter, [n] to take the nth match (from 1) under each
    // parent. A leading '//' searches the whole document. The first name is the root element.
    std::vector<size_t> Select(const std::string& path) const;

    // Edits return the new text; the document must be parsed again before the next edit (nodes move).
    std::string SetAttr(size_t node, const std::string& name, const std::string& value, bool escape = true) const;
    std::string RemoveAttr(size_t node, const std::string& name) const;
    std::string SetText(size_t node, const std::string& value, bool escape = true) const;
    std::string Remove(size_t node) const;
    // where: "append" or "prepend" (as a child), "before" or "after" (as a sibling)
    std::string Insert(size_t node, const std::string& where, const std::string& fragment) const;

    static std::string Escape(const std::string& s, bool for_attribute);

private:
    std::string text_;
    std::vector<XmlNode> nodes_;  // nodes_[0] stands for the document itself
};

}  // namespace dcmod
