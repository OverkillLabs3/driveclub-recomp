// SPDX-License-Identifier: GPL-2.0-or-later
#include "csv.hpp"

namespace dcmod {

CsvRecord CsvDoc::ParseRecord(const std::string& raw, char delimiter) {
    CsvRecord rec;
    rec.raw = raw;
    size_t i = 0;
    for (;;) {
        CsvField f;
        f.begin = i;
        if (i < raw.size() && raw[i] == '"') {
            f.quoted = true;
            ++i;
            while (i < raw.size()) {
                if (raw[i] == '"') {
                    if (i + 1 < raw.size() && raw[i + 1] == '"') {
                        f.value.push_back('"');
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                f.value.push_back(raw[i++]);
            }
            // Anything between the closing quote and the delimiter belongs to the value.
            while (i < raw.size() && raw[i] != delimiter) f.value.push_back(raw[i++]);
        } else {
            while (i < raw.size() && raw[i] != delimiter) f.value.push_back(raw[i++]);
        }
        f.end = i;
        rec.fields.push_back(std::move(f));
        if (i >= raw.size()) break;
        ++i;  // the delimiter
        if (i == raw.size()) {
            CsvField last;
            last.begin = last.end = i;
            rec.fields.push_back(std::move(last));
            break;
        }
    }
    return rec;
}

CsvDoc CsvDoc::Parse(const std::string& text) {
    CsvDoc doc;
    // The delimiter is whichever of , ; and tab is most common in the first line.
    {
        size_t counts[3] = {0, 0, 0};
        bool in_quotes = false;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '"') in_quotes = !in_quotes;
            else if (!in_quotes && c == '\n') break;
            else if (!in_quotes && c == ',') ++counts[0];
            else if (!in_quotes && c == ';') ++counts[1];
            else if (!in_quotes && c == '\t') ++counts[2];
        }
        const char delims[3] = {',', ';', '\t'};
        size_t best = 0;
        for (size_t k = 1; k < 3; ++k) {
            if (counts[k] > counts[best]) best = k;
        }
        doc.delimiter = delims[best];
    }
    size_t start = 0;
    bool in_quotes = false;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size();
        if (!end && text[i] == '"') in_quotes = !in_quotes;
        if (end || (text[i] == '\n' && !in_quotes)) {
            if (end && start == text.size()) {
                doc.final_newline_ = !text.empty();
                break;
            }
            doc.rows.push_back(ParseRecord(text.substr(start, i - start), doc.delimiter));
            if (end) doc.final_newline_ = false;
            start = i + 1;
        }
    }
    return doc;
}

std::string CsvDoc::Serialize() const {
    std::string out;
    for (size_t i = 0; i < rows.size(); ++i) {
        out += rows[i].raw;
        if (i + 1 < rows.size() || final_newline_) out.push_back('\n');
    }
    return out;
}

int CsvDoc::Column(const std::string& name) const {
    if (rows.empty()) return -1;
    const std::string want = Lower(name);
    for (size_t i = 0; i < rows[0].fields.size(); ++i) {
        if (Lower(rows[0].fields[i].value) == want) return int(i);
    }
    return -1;
}

const std::string& CsvDoc::Get(size_t row, size_t col) const {
    static const std::string empty;
    if (row >= rows.size() || col >= rows[row].fields.size()) return empty;
    return rows[row].fields[col].value;
}

std::string CsvDoc::Cell(const std::string& value, bool quoted) const {
    bool needs = quoted;
    if (!needs) {
        for (char c : value) {
            if (c == delimiter || c == '"' || c == '\n' || c == '\r') needs = true;
        }
        if (!value.empty() && (value.front() == ' ' || value.back() == ' ')) needs = true;
    }
    if (!needs) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void CsvDoc::Set(size_t row, size_t col, const std::string& value) {
    CsvRecord& rec = rows.at(row);
    bool quoted = false;
    if (!rec.fields.empty()) quoted = rec.fields[std::min(col, rec.fields.size() - 1)].quoted;
    while (rec.fields.size() <= col) {
        rec.raw.push_back(delimiter);
        rec = ParseRecord(rec.raw, delimiter);
    }
    const CsvField& f = rec.fields[col];
    std::string raw = rec.raw.substr(0, f.begin) + Cell(value, quoted) + rec.raw.substr(f.end);
    rec = ParseRecord(raw, delimiter);
}

size_t CsvDoc::Add(const std::vector<std::string>& values, long like, long after) {
    if (rows.empty()) throw Error("the table has no header row");
    const size_t width = rows[0].fields.size();
    std::string raw;
    for (size_t c = 0; c < width; ++c) {
        bool quoted = false;
        if (like >= 0 && size_t(like) < rows.size() && c < rows[size_t(like)].fields.size()) quoted = rows[size_t(like)].fields[c].quoted;
        else if (like < 0 && rows.size() > 1 && c < rows[1].fields.size()) quoted = rows[1].fields[c].quoted;
        if (c) raw.push_back(delimiter);
        raw += Cell(c < values.size() ? values[c] : std::string(), quoted);
    }
    size_t at = after < 0 ? rows.size() : size_t(after) + 1;
    if (at > rows.size()) at = rows.size();
    rows.insert(rows.begin() + std::ptrdiff_t(at), ParseRecord(raw, delimiter));
    return at;
}

void CsvDoc::Remove(size_t row) {
    if (row == 0 || row >= rows.size()) throw Error("there is no such row to remove");
    rows.erase(rows.begin() + std::ptrdiff_t(row));
}

}  // namespace dcmod
