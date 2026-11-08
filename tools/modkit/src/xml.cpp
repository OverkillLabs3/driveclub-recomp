// SPDX-License-Identifier: GPL-2.0-or-later
#include "xml.hpp"

#include <algorithm>

namespace dcmod {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsNameEnd(char c) { return IsSpace(c) || c == '/' || c == '>' || c == '=' || c == '<'; }

[[noreturn]] void Fail(const std::string& text, size_t at, const std::string& what) {
    size_t line = 1;
    for (size_t i = 0; i < at && i < text.size(); ++i) {
        if (text[i] == '\n') ++line;
    }
    throw Error("XML, line " + std::to_string(line) + ": " + what);
}

size_t LineStart(const std::string& s, size_t at) {
    while (at > 0 && s[at - 1] != '\n') --at;
    return at;
}

bool OnlySpace(const std::string& s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (!IsSpace(s[i])) return false;
    }
    return true;
}

std::string IndentOf(const std::string& s, size_t at) {
    const size_t ls = LineStart(s, at);
    size_t e = ls;
    while (e < s.size() && (s[e] == ' ' || s[e] == '\t')) ++e;
    return s.substr(ls, e - ls);
}

struct Step {
    std::string name;  // "*" for any
    std::vector<std::pair<std::string, std::string>> attrs;  // [@a='v']
    std::vector<std::string> has_attrs;                      // [@a]
    int index = 0;                                           // [n], 0 for none
};

std::vector<Step> ParsePath(const std::string& path, bool& anywhere) {
    std::vector<Step> steps;
    size_t i = 0;
    anywhere = false;
    if (path.compare(0, 2, "//") == 0) {
        anywhere = true;
        i = 2;
    } else if (!path.empty() && path[0] == '/') {
        i = 1;
    }
    while (i < path.size()) {
        Step st;
        while (i < path.size() && path[i] != '/' && path[i] != '[') st.name.push_back(path[i++]);
        while (i < path.size() && path[i] == '[') {
            const size_t close = path.find(']', i);
            if (close == std::string::npos) throw Error("the path \"" + path + "\" has a '[' without a ']'");
            std::string pred = path.substr(i + 1, close - i - 1);
            i = close + 1;
            if (!pred.empty() && pred[0] == '@') {
                const size_t eq = pred.find('=');
                if (eq == std::string::npos) {
                    st.has_attrs.push_back(pred.substr(1));
                } else {
                    std::string v = pred.substr(eq + 1);
                    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                    st.attrs.emplace_back(pred.substr(1, eq - 1), v);
                }
            } else if (!pred.empty() && pred.find_first_not_of("0123456789") == std::string::npos) {
                st.index = std::stoi(pred);
            } else {
                throw Error("the path \"" + path + "\" has a condition that is not understood: [" + pred + "]");
            }
        }
        if (st.name.empty()) throw Error("the path \"" + path + "\" has an empty name");
        steps.push_back(std::move(st));
        if (i < path.size() && path[i] == '/') ++i;
    }
    if (steps.empty()) throw Error("the path is empty");
    return steps;
}

bool Matches(const XmlNode& n, const Step& st) {
    if (st.name != "*" && n.name != st.name) return false;
    for (const std::string& a : st.has_attrs) {
        if (!n.Attr(a)) return false;
    }
    for (const auto& [a, v] : st.attrs) {
        const XmlAttr* at = n.Attr(a);
        if (!at || at->value != v) return false;
    }
    return true;
}

}  // namespace

std::string XmlDoc::Escape(const std::string& s, bool for_attribute) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += for_attribute ? "&quot;" : "\""; break;
            default: out.push_back(c);
        }
    }
    return out;
}

XmlDoc XmlDoc::Parse(const std::string& text) {
    XmlDoc doc;
    doc.text_ = text;
    doc.nodes_.emplace_back();  // the document
    std::vector<size_t> open{0};
    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n) {
        if (text[pos] != '<') {
            const size_t next = text.find('<', pos);
            pos = next == std::string::npos ? n : next;
            continue;
        }
        if (text.compare(pos, 4, "<!--") == 0) {
            const size_t e = text.find("-->", pos + 4);
            if (e == std::string::npos) Fail(text, pos, "a comment is not closed");
            pos = e + 3;
        } else if (text.compare(pos, 9, "<![CDATA[") == 0) {
            const size_t e = text.find("]]>", pos + 9);
            if (e == std::string::npos) Fail(text, pos, "a CDATA section is not closed");
            pos = e + 3;
        } else if (text.compare(pos, 2, "<?") == 0) {
            const size_t e = text.find("?>", pos + 2);
            if (e == std::string::npos) Fail(text, pos, "a processing instruction is not closed");
            pos = e + 2;
        } else if (text.compare(pos, 2, "<!") == 0) {
            int depth = 0;
            size_t i = pos + 2;
            for (; i < n; ++i) {
                if (text[i] == '[') ++depth;
                else if (text[i] == ']') --depth;
                else if (text[i] == '>' && depth <= 0) break;
            }
            if (i >= n) Fail(text, pos, "a declaration is not closed");
            pos = i + 1;
        } else if (text.compare(pos, 2, "</") == 0) {
            size_t i = pos + 2;
            std::string name;
            while (i < n && !IsNameEnd(text[i])) name.push_back(text[i++]);
            while (i < n && IsSpace(text[i])) ++i;
            if (i >= n || text[i] != '>') Fail(text, pos, "a closing tag is not finished");
            // The game's own files are not always well formed (a missing closing tag, for one), so an element that
            // was left open is closed where its parent closes, and a stray closing tag is ignored.
            size_t k = open.size();
            while (k > 1 && doc.nodes_[open[k - 1]].name != name) --k;
            if (k > 1) {
                while (open.size() > k) {
                    XmlNode& left = doc.nodes_[open.back()];
                    left.content_end = left.end = pos;
                    open.pop_back();
                }
                XmlNode& node = doc.nodes_[open.back()];
                node.content_end = pos;
                node.end = i + 1;
                open.pop_back();
            }
            pos = i + 1;
        } else {
            XmlNode node;
            node.begin = pos;
            size_t i = pos + 1;
            while (i < n && !IsNameEnd(text[i])) node.name.push_back(text[i++]);
            if (node.name.empty()) Fail(text, pos, "a '<' is not followed by a name");
            for (;;) {
                while (i < n && IsSpace(text[i])) ++i;
                if (i >= n) Fail(text, pos, "the tag <" + node.name + "> is not finished");
                if (text[i] == '>') {
                    ++i;
                    break;
                }
                if (text[i] == '/' && i + 1 < n && text[i + 1] == '>') {
                    node.self_closing = true;
                    i += 2;
                    break;
                }
                XmlAttr a;
                a.begin = i;
                while (i < n && !IsNameEnd(text[i])) a.name.push_back(text[i++]);
                if (a.name.empty()) Fail(text, i, "an attribute name is expected in <" + node.name + ">");
                while (i < n && IsSpace(text[i])) ++i;
                if (i >= n || text[i] != '=') Fail(text, i, "the attribute " + a.name + " has no '='");
                ++i;
                while (i < n && IsSpace(text[i])) ++i;
                if (i >= n || (text[i] != '"' && text[i] != '\'')) Fail(text, i, "the value of " + a.name + " is not in quotes");
                const char q = text[i++];
                a.value_begin = i;
                while (i < n && text[i] != q) ++i;
                if (i >= n) Fail(text, a.value_begin, "the value of " + a.name + " is not closed");
                a.value_end = i;
                a.value = text.substr(a.value_begin, a.value_end - a.value_begin);
                ++i;
                a.end = i;
                node.attrs.push_back(std::move(a));
            }
            node.start_end = i;
            node.parent = int(open.back());
            const size_t index = doc.nodes_.size();
            if (node.self_closing) {
                node.end = i;
                node.content_begin = node.content_end = i;
            } else {
                node.content_begin = i;
            }
            doc.nodes_[open.back()].children.push_back(index);
            doc.nodes_.push_back(std::move(node));
            if (!doc.nodes_.back().self_closing) open.push_back(index);
            pos = i;
        }
    }
    while (open.size() > 1) {
        XmlNode& left = doc.nodes_[open.back()];
        left.content_end = left.end = n;
        open.pop_back();
    }
    return doc;
}

std::vector<size_t> XmlDoc::Select(const std::string& path) const {
    bool anywhere = false;
    const std::vector<Step> steps = ParsePath(path, anywhere);
    std::vector<size_t> current{0};
    for (size_t s = 0; s < steps.size(); ++s) {
        std::vector<size_t> next;
        for (size_t parent : current) {
            std::vector<size_t> candidates;
            if (anywhere && s == 0) {
                // every descendant
                std::vector<size_t> stack{parent};
                while (!stack.empty()) {
                    const size_t cur = stack.back();
                    stack.pop_back();
                    for (size_t k = nodes_[cur].children.size(); k-- > 0;) stack.push_back(nodes_[cur].children[k]);
                    if (cur != parent) candidates.push_back(cur);
                }
                std::sort(candidates.begin(), candidates.end());
            } else {
                candidates = nodes_[parent].children;
            }
            int seen = 0;
            for (size_t c : candidates) {
                if (!Matches(nodes_[c], steps[s])) continue;
                ++seen;
                if (steps[s].index == 0 || steps[s].index == seen) next.push_back(c);
            }
        }
        current = std::move(next);
        if (current.empty()) break;
    }
    return current;
}

std::string XmlDoc::SetAttr(size_t node, const std::string& name, const std::string& value, bool escape) const {
    const XmlNode& n = nodes_.at(node);
    const std::string v = escape ? Escape(value, true) : value;
    std::string out = text_;
    if (const XmlAttr* a = n.Attr(name)) {
        out.replace(a->value_begin, a->value_end - a->value_begin, v);
        return out;
    }
    // A new attribute goes after the last one, or right after the element's name.
    const size_t at = n.attrs.empty() ? n.begin + 1 + n.name.size() : n.attrs.back().end;
    out.insert(at, " " + name + "=\"" + v + "\"");
    return out;
}

std::string XmlDoc::RemoveAttr(size_t node, const std::string& name) const {
    const XmlNode& n = nodes_.at(node);
    const XmlAttr* a = n.Attr(name);
    if (!a) throw Error("the element <" + n.name + "> has no attribute " + name);
    size_t from = a->begin;
    while (from > n.begin && IsSpace(text_[from - 1])) --from;
    std::string out = text_;
    out.erase(from, a->end - from);
    return out;
}

std::string XmlDoc::SetText(size_t node, const std::string& value, bool escape) const {
    const XmlNode& n = nodes_.at(node);
    const std::string v = escape ? Escape(value, false) : value;
    std::string out = text_;
    if (n.self_closing) {
        // <a b="1"/> becomes <a b="1">text</a>
        size_t slash = n.start_end - 2;
        while (slash > n.begin && IsSpace(out[slash - 1])) --slash;
        out.replace(slash, n.start_end - slash, ">" + v + "</" + n.name + ">");
        return out;
    }
    out.replace(n.content_begin, n.content_end - n.content_begin, v);
    return out;
}

std::string XmlDoc::Remove(size_t node) const {
    const XmlNode& n = nodes_.at(node);
    if (node == 0) throw Error("the document itself cannot be removed");
    size_t from = n.begin, to = n.end;
    const size_t ls = LineStart(text_, from);
    // When the element is alone on its lines, take the whole lines.
    if (OnlySpace(text_, ls, from)) {
        size_t e = to;
        while (e < text_.size() && (text_[e] == ' ' || text_[e] == '\t')) ++e;
        if (e >= text_.size() || text_[e] == '\n') {
            from = ls;
            to = e < text_.size() ? e + 1 : e;
        }
    }
    std::string out = text_;
    out.erase(from, to - from);
    return out;
}

std::string XmlDoc::Insert(size_t node, const std::string& where, const std::string& fragment) const {
    const XmlNode& n = nodes_.at(node);
    std::string out = text_;
    if (where == "before" || where == "after") {
        if (node == 0) throw Error("a sibling of the document cannot be added");
        const std::string indent = IndentOf(text_, n.begin);
        const bool own_line = OnlySpace(text_, LineStart(text_, n.begin), n.begin);
        if (where == "before") {
            out.insert(n.begin, fragment + (own_line ? "\n" + indent : std::string()));
        } else {
            out.insert(n.end, (own_line ? "\n" + indent : std::string()) + fragment);
        }
        return out;
    }
    if (where != "append" && where != "prepend") throw Error("unknown place \"" + where + "\" (use append, prepend, before or after)");
    if (n.self_closing) {
        // <a/> must become <a></a> first
        size_t slash = n.start_end - 2;
        while (slash > n.begin && IsSpace(out[slash - 1])) --slash;
        out.replace(slash, n.start_end - slash, "></" + n.name + ">");
        const XmlDoc again = Parse(out);
        // the same element is now the node that starts at the same place
        for (size_t i = 1; i < again.nodes_.size(); ++i) {
            if (again.nodes_[i].begin == n.begin) return again.Insert(i, where, fragment);
        }
        throw Error("internal error while opening an empty element");
    }
    const std::string parent_indent = IndentOf(text_, n.begin);
    std::string child_indent = parent_indent + "\t";
    if (!n.children.empty()) child_indent = IndentOf(text_, nodes_[n.children.front()].begin);
    else if (parent_indent.find('\t') == std::string::npos) child_indent = parent_indent + "  ";
    if (where == "append") {
        const size_t closing_line = LineStart(text_, n.content_end);
        if (OnlySpace(text_, closing_line, n.content_end) && closing_line >= n.content_begin) {
            out.insert(closing_line, child_indent + fragment + "\n");
        } else {
            out.insert(n.content_end, fragment);
        }
    } else {
        // after the start tag; when the first child is on its own line the new element gets a line too
        if (!n.children.empty() && OnlySpace(text_, LineStart(text_, nodes_[n.children.front()].begin), nodes_[n.children.front()].begin)) {
            out.insert(LineStart(text_, nodes_[n.children.front()].begin), child_indent + fragment + "\n");
        } else {
            out.insert(n.content_begin, fragment);
        }
    }
    return out;
}

}  // namespace dcmod
