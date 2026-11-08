// SPDX-License-Identifier: GPL-2.0-or-later
// CSV tables that can be edited cell by cell. Every record keeps its original text, and a change only replaces the
// text of the cells that change, so the rest of the file stays byte for byte as it was (quotes, spacing and all).
#pragma once

#include "textfile.hpp"

namespace dcmod {

struct CsvField {
    size_t begin = 0, end = 0;  // where the field is in the record's text, quotes included
    bool quoted = false;
    std::string value;          // the content, with quotes removed and "" turned into "
};

struct CsvRecord {
    std::string raw;  // the record as written, without its line break (it may hold line breaks inside quotes)
    std::vector<CsvField> fields;
};

class CsvDoc {
public:
    static CsvDoc Parse(const std::string& text);  // line breaks are '\n'
    std::string Serialize() const;

    char delimiter = ',';
    std::vector<CsvRecord> rows;  // rows[0] is the header

    int Column(const std::string& name) const;  // -1 when there is no such column
    const std::string& Get(size_t row, size_t col) const;
    void Set(size_t row, size_t col, const std::string& value);
    // A new row with the given values by column index (missing ones are empty). With `like` >= 0 the cell styles of
    // that row are used. Inserted after row `after`, or at the end when it is negative. Returns the new row's index.
    size_t Add(const std::vector<std::string>& values, long like, long after);
    void Remove(size_t row);

private:
    static CsvRecord ParseRecord(const std::string& raw, char delimiter);
    std::string Cell(const std::string& value, bool quoted) const;
    bool final_newline_ = true;
};

}  // namespace dcmod
