// keywords.cpp — keyword rules: matching, the named-colour table, CSV import.
//
// See keywords.h for the contract. The two things worth knowing before reading
// on are that matching is whole-word over Normalize()d text, and that the rule
// list is kept pre-sorted so that "first match wins" is also "best match wins".

#include "keywords.h"

#include <algorithm>
#include <set>

namespace rc {
namespace keywords {

const COLORREF kUnmatchedColor = RGB(0x8E, 0x8E, 0x93);
const wchar_t* const kUncategorized = L"Uncategorized";

namespace {

// The whole state of this module. A single rule set is in force at a time; it
// is only ever replaced wholesale from the UI thread, so no lock is needed.
std::vector<KeywordRule> g_rules;
std::wstring g_source;

int CountWords(const std::wstring& normalized) {
    int words = 0;
    bool inWord = false;
    for (wchar_t c : normalized) {
        if (c == L' ') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
    }
    return words;
}

// Removes spaces, so "light gray" and "lightgray" reach the same table entry.
std::wstring Squeeze(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c != L' ') out.push_back(c);
    }
    return out;
}

struct NamedColor {
    const wchar_t* name;
    COLORREF color;
};

// Section 4.3 of the spec, verbatim, aliases included. These are the macOS
// system colours; they are hard-coded rather than read from the OS because a
// CSV that says "blue" must mean the same blue on every machine.
const NamedColor kNamedColors[] = {
    {L"red", RGB(0xFF, 0x3B, 0x30)},
    {L"orange", RGB(0xFF, 0x95, 0x00)},
    {L"yellow", RGB(0xFF, 0xCC, 0x00)},
    {L"green", RGB(0x28, 0xCD, 0x41)},
    {L"mint", RGB(0x00, 0xC7, 0xBE)},
    {L"teal", RGB(0x30, 0xB0, 0xC7)},
    {L"cyan", RGB(0x32, 0xAD, 0xE6)},
    {L"aqua", RGB(0x32, 0xAD, 0xE6)},
    {L"blue", RGB(0x00, 0x7A, 0xFF)},
    {L"indigo", RGB(0x58, 0x56, 0xD6)},
    {L"purple", RGB(0xAF, 0x52, 0xDE)},
    {L"violet", RGB(0xAF, 0x52, 0xDE)},
    {L"pink", RGB(0xFF, 0x2D, 0x55)},
    {L"magenta", RGB(0xFF, 0x2D, 0x55)},
    {L"fuchsia", RGB(0xFF, 0x2D, 0x55)},
    {L"brown", RGB(0xA2, 0x84, 0x5E)},
    {L"gray", RGB(0x8E, 0x8E, 0x93)},
    {L"grey", RGB(0x8E, 0x8E, 0x93)},
    {L"light gray", RGB(0xC7, 0xC7, 0xCC)},
    {L"light grey", RGB(0xC7, 0xC7, 0xCC)},
    {L"silver", RGB(0xC7, 0xC7, 0xCC)},
    {L"dark gray", RGB(0x48, 0x48, 0x4A)},
    {L"dark grey", RGB(0x48, 0x48, 0x4A)},
    {L"charcoal", RGB(0x48, 0x48, 0x4A)},
    {L"black", RGB(0x00, 0x00, 0x00)},
    {L"white", RGB(0xFF, 0xFF, 0xFF)},
};

// ------------------------------------------------------------------ the sample

struct SampleEntry {
    const wchar_t* category;
    const wchar_t* colorName;
    const wchar_t* keyword;
};

// 42 rules, 6 categories, spec section 4.4. Order matters only in that it is
// the tie-breaker of last resort when two rules are otherwise equal.
const SampleEntry kSample[] = {
    // Focus Work | Learn
    {L"Focus Work | Learn", L"blue", L"focus"},
    {L"Focus Work | Learn", L"blue", L"work"},
    {L"Focus Work | Learn", L"blue", L"learn"},
    {L"Focus Work | Learn", L"blue", L"deep tasks"},
    {L"Focus Work | Learn", L"blue", L"writing"},
    {L"Focus Work | Learn", L"blue", L"coding"},
    {L"Focus Work | Learn", L"blue", L"project creation"},
    // Meetings | Urgency
    {L"Meetings | Urgency", L"red", L"meeting"},
    {L"Meetings | Urgency", L"red", L"deadline"},
    {L"Meetings | Urgency", L"red", L"call"},
    {L"Meetings | Urgency", L"red", L"calls"},
    {L"Meetings | Urgency", L"red", L"sync"},
    {L"Meetings | Urgency", L"red", L"planning"},
    {L"Meetings | Urgency", L"red", L"syncs"},
    {L"Meetings | Urgency", L"red", L"client syncs"},
    {L"Meetings | Urgency", L"red", L"standup"},
    {L"Meetings | Urgency", L"red", L"interview"},
    {L"Meetings | Urgency", L"red", L"high-priority deadlines"},
    {L"Meetings | Urgency", L"red", L"training"},
    // Health | Rest
    {L"Health | Rest", L"green", L"sleep"},
    {L"Health | Rest", L"green", L"meal"},
    {L"Health | Rest", L"green", L"nap"},
    {L"Health | Rest", L"green", L"lunch"},
    {L"Health | Rest", L"green", L"exercise"},
    {L"Health | Rest", L"green", L"me-time"},
    {L"Health | Rest", L"green", L"gym sessions"},
    {L"Health | Rest", L"green", L"walks"},
    {L"Health | Rest", L"green", L"mental breaks"},
    // Admin | Errands
    {L"Admin | Errands", L"yellow", L"email clearing"},
    {L"Admin | Errands", L"yellow", L"update"},
    {L"Admin | Errands", L"yellow", L"to-do list"},
    {L"Admin | Errands", L"yellow", L"minor chores"},
    // Personal | Growth
    {L"Personal | Growth", L"purple", L"lecture"},
    {L"Personal | Growth", L"purple", L"meal prep"},
    {L"Personal | Growth", L"purple", L"family time"},
    {L"Personal | Growth", L"purple", L"reading"},
    {L"Personal | Growth", L"purple", L"self development"},
    {L"Personal | Growth", L"purple", L"personal development"},
    {L"Personal | Growth", L"purple", L"church"},
    // Travel | Buffers
    {L"Travel | Buffers", L"teal", L"commute"},
    {L"Travel | Buffers", L"teal", L"buffer"},
    {L"Travel | Buffers", L"teal", L"out of office"},
};

// ---------------------------------------------------------------- CSV helpers

// CRLF and lone CR both become LF before anything else looks at the text, so
// the row parser only ever has one line terminator to think about.
std::wstring FlattenLineEndings(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') continue;  // CRLF: keep the LF
            out.push_back(L'\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

wchar_t DetectDelimiter(const std::wstring& text) {
    const wchar_t candidates[] = {L',', L';', L'\t'};
    size_t counts[3] = {0, 0, 0};
    int line = 0;
    for (size_t i = 0; i < text.size() && line < 4; ++i) {
        wchar_t c = text[i];
        if (c == L'\n') {
            ++line;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            if (c == candidates[k]) ++counts[k];
        }
    }
    int best = 0;
    for (int k = 1; k < 3; ++k) {
        if (counts[k] > counts[best]) best = k;
    }
    return counts[best] > 0 ? candidates[best] : L',';
}

// RFC 4180 subset: quoted fields, doubled quotes inside them, nothing else.
// Anything malformed is read as literal text rather than rejected, because a
// half-quoted spreadsheet export should still import the rows it can.
std::vector<std::vector<std::wstring>> ParseRows(const std::wstring& text, wchar_t delim) {
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring field;
    bool quoted = false;
    bool any = false;

    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (quoted) {
            if (c == L'"') {
                if (i + 1 < text.size() && text[i + 1] == L'"') {
                    field.push_back(L'"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
            any = true;
            continue;
        }
        if (c == L'"') {
            quoted = true;
            any = true;
        } else if (c == delim) {
            row.push_back(field);
            field.clear();
            any = true;
        } else if (c == L'\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
            any = false;
        } else {
            field.push_back(c);
            any = true;
        }
    }
    if (any || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

std::wstring FieldAt(const std::vector<std::wstring>& row, int index) {
    if (index < 0 || static_cast<size_t>(index) >= row.size()) return std::wstring();
    return Trim(row[static_cast<size_t>(index)]);
}

bool RowIsBlank(const std::vector<std::wstring>& row) {
    for (const std::wstring& f : row) {
        if (!Trim(f).empty()) return false;
    }
    return true;
}

int FindHeaderContaining(const std::vector<std::wstring>& headers, const std::wstring& needle) {
    for (size_t i = 0; i < headers.size(); ++i) {
        if (headers[i].find(needle) != std::wstring::npos) return static_cast<int>(i);
    }
    return -1;
}

int FindHeaderStartingWith(const std::vector<std::wstring>& headers,
                           const std::wstring* prefixes,
                           size_t count) {
    for (size_t i = 0; i < headers.size(); ++i) {
        for (size_t k = 0; k < count; ++k) {
            if (StartsWith(headers[i], prefixes[k])) return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring JoinRow(const std::vector<std::wstring>& row) {
    std::wstring out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i) out += L" | ";
        out += Trim(row[i]);
    }
    return out;
}

std::wstring DelimiterName(wchar_t d) {
    if (d == L';') return L"semicolon";
    if (d == L'\t') return L"tab";
    return L"comma";
}

std::wstring ColumnName(int index) {
    return index < 0 ? std::wstring(L"none") : Format(L"%d", index + 1);
}

// A CSV field only needs quoting when it carries the delimiter, a quote or a
// newline; quoting everything would make the sample file harder to read.
std::wstring CsvEscape(const std::wstring& s) {
    bool needs = s.find_first_of(L",\"\n\r") != std::wstring::npos;
    if (!needs) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out.push_back(L'"');
        out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

}  // namespace

// -------------------------------------------------------------- rule storage

const std::vector<KeywordRule>& Rules() { return g_rules; }

void SetRules(std::vector<KeywordRule> rules, const std::wstring& sourceName) {
    // Fill in anything the caller left blank, and drop rules that normalize to
    // nothing: an empty needle is contained in every title and would paint the
    // whole calendar one colour.
    std::vector<KeywordRule> kept;
    kept.reserve(rules.size());
    for (KeywordRule& r : rules) {
        if (r.normalized.empty()) r.normalized = Normalize(r.keyword);
        if (r.normalized.empty()) continue;
        r.wordCount = CountWords(r.normalized);
        kept.push_back(std::move(r));
    }

    // Longest phrase must win, so "meal prep" beats "meal" on "Meal prep for
    // the week" while "Prep the meal" still finds "meal". Word count first,
    // then raw length, and a stable sort so the file's own order settles the
    // rest -- an unstable sort here would make identical imports colour
    // differently from run to run.
    std::stable_sort(kept.begin(), kept.end(), [](const KeywordRule& a, const KeywordRule& b) {
        if (a.wordCount != b.wordCount) return a.wordCount > b.wordCount;
        return a.normalized.size() > b.normalized.size();
    });

    g_rules = std::move(kept);
    g_source = sourceName;
}

void Clear() {
    g_rules.clear();
    g_source.clear();
}

const std::wstring& SourceName() { return g_source; }

// ------------------------------------------------------------------ matching

void Apply(std::vector<CalEvent>& events) {
    if (g_rules.empty()) return;
    for (CalEvent& ev : events) {
        const std::wstring normalized = Normalize(ev.title);
        if (normalized.empty()) continue;
        for (const KeywordRule& rule : g_rules) {
            if (ContainsWord(normalized, rule.normalized)) {
                ev.color = rule.color;
                ev.category = rule.category;
                break;  // pre-sorted, so the first match is the longest match
            }
        }
    }
}

std::vector<CategorySummary> Categories() {
    std::vector<CategorySummary> out;
    for (const KeywordRule& rule : g_rules) {
        const std::wstring& name = rule.category.empty() ? std::wstring(kUncategorized) : rule.category;
        bool found = false;
        for (CategorySummary& s : out) {
            if (s.name == name) {
                ++s.ruleCount;
                found = true;
                break;
            }
        }
        if (!found) {
            CategorySummary s;
            s.name = name;
            s.color = rule.color;
            s.ruleCount = 1;
            out.push_back(s);
        }
    }
    return out;
}

COLORREF ColorForCategory(const std::wstring& category) {
    for (const KeywordRule& rule : g_rules) {
        if (rule.category == category) return rule.color;
    }
    return kUnmatchedColor;
}

// ------------------------------------------------------------------- colours

bool ParseColorToken(const std::wstring& token, COLORREF* out) {
    if (!out) return false;
    const std::wstring trimmed = Trim(token);
    if (trimmed.empty()) return false;

    const std::wstring key = Normalize(trimmed);
    if (!key.empty()) {
        const std::wstring squeezed = Squeeze(key);
        for (const NamedColor& nc : kNamedColors) {
            const std::wstring name(nc.name);
            if (key == name || squeezed == Squeeze(name)) {
                *out = nc.color;
                return true;
            }
        }
    }
    return ParseHexColor(trimmed, out);
}

// -------------------------------------------------------------- the built-in

std::vector<KeywordRule> SampleRules() {
    std::vector<KeywordRule> rules;
    rules.reserve(sizeof(kSample) / sizeof(kSample[0]));
    for (const SampleEntry& e : kSample) {
        KeywordRule r;
        r.category = e.category;
        r.colorName = e.colorName;
        r.keyword = e.keyword;
        r.normalized = Normalize(r.keyword);
        r.wordCount = CountWords(r.normalized);
        COLORREF c = kUnmatchedColor;
        ParseColorToken(r.colorName, &c);  // every sample name is in the table
        r.color = c;
        rules.push_back(std::move(r));
    }
    return rules;
}

std::wstring SampleCsv() {
    std::wstring out = L"category,color,keyword\r\n";
    for (const SampleEntry& e : kSample) {
        out += CsvEscape(e.category);
        out += L',';
        out += CsvEscape(e.colorName);
        out += L',';
        out += CsvEscape(e.keyword);
        out += L"\r\n";
    }
    return out;
}

// ---------------------------------------------------------------- CSV import

CsvImportReport ParseCsv(const std::wstring& text, std::vector<KeywordRule>* out) {
    CsvImportReport report;

    std::wstring body = FlattenLineEndings(text);
    if (!body.empty() && body[0] == static_cast<wchar_t>(0xFEFF)) body.erase(0, 1);

    const wchar_t delim = DetectDelimiter(body);
    std::vector<std::vector<std::wstring>> rows = ParseRows(body, delim);

    // Drop spacer rows before anything is counted or inferred; they are common
    // in hand-maintained spreadsheets and mean nothing.
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [](const std::vector<std::wstring>& r) { return RowIsBlank(r); }),
               rows.end());

    size_t columns = 0;
    for (const std::vector<std::wstring>& r : rows) columns = (std::max)(columns, r.size());

    auto fail = [&](const std::wstring& why, int colColumn, int keyColumn, int catColumn,
                    size_t firstDataRow) {
        report.ok = false;
        report.error = why;
        std::wstring first =
            firstDataRow < rows.size() ? JoinRow(rows[firstDataRow]) : std::wstring(L"(none)");
        report.diagnostics =
            Format(L"Delimiter: %s\r\nColumns: %d\r\nCategory column: %s\r\n"
                   L"Colour column: %s\r\nKeyword column: %s\r\nFirst data row: %s",
                   DelimiterName(delim).c_str(), static_cast<int>(columns),
                   ColumnName(catColumn).c_str(), ColumnName(colColumn).c_str(),
                   ColumnName(keyColumn).c_str(), first.c_str());
        return report;
    };

    if (rows.empty() || columns < 2) {
        return fail(L"That file has no usable rows.", -1, -1, -1, 0);
    }

    // --- column identification, in the order the spec lays down

    std::vector<std::wstring> headers;
    for (size_t i = 0; i < columns; ++i) headers.push_back(Lower(FieldAt(rows[0], static_cast<int>(i))));

    // A file with separate color_name / color_hex columns must take the hex
    // one, so "contains hex" is tried before "starts with color".
    int colColumn = FindHeaderContaining(headers, L"hex");
    if (colColumn < 0) {
        const std::wstring colorPrefixes[] = {L"color", L"colour"};
        colColumn = FindHeaderStartingWith(headers, colorPrefixes, 2);
    }
    const std::wstring keywordPrefixes[] = {L"keyword", L"term", L"phrase"};
    int keyColumn = FindHeaderStartingWith(headers, keywordPrefixes, 3);
    const std::wstring categoryPrefixes[] = {L"category", L"group"};
    int catColumn = FindHeaderStartingWith(headers, categoryPrefixes, 2);

    size_t firstData = 1;
    if (colColumn < 0 && keyColumn < 0) {
        // Neither of the two columns that matter was named, so there is no
        // header row: row 1 is data, and the columns are worked out from what
        // they contain rather than what they are called.
        firstData = 0;
        const size_t sample = (std::min)(rows.size(), static_cast<size_t>(20));

        size_t bestColorHits = 0;
        for (size_t c = 0; c < columns; ++c) {
            size_t hits = 0;
            for (size_t r = 0; r < sample; ++r) {
                COLORREF ignored = 0;
                if (ParseColorToken(FieldAt(rows[r], static_cast<int>(c)), &ignored)) ++hits;
            }
            if (hits > bestColorHits) {
                bestColorHits = hits;
                colColumn = static_cast<int>(c);
            }
        }
        if (bestColorHits == 0) colColumn = -1;

        // Keywords are the column that repeats itself least; categories the one
        // that repeats itself most.
        size_t mostDistinct = 0;
        size_t fewestDistinct = static_cast<size_t>(-1);
        for (size_t c = 0; c < columns; ++c) {
            if (static_cast<int>(c) == colColumn) continue;
            std::set<std::wstring> distinct;
            for (size_t r = 0; r < sample; ++r) {
                std::wstring v = Lower(FieldAt(rows[r], static_cast<int>(c)));
                if (!v.empty()) distinct.insert(v);
            }
            if (distinct.size() > mostDistinct) {
                mostDistinct = distinct.size();
                keyColumn = static_cast<int>(c);
            }
        }
        for (size_t c = 0; c < columns; ++c) {
            if (static_cast<int>(c) == colColumn || static_cast<int>(c) == keyColumn) continue;
            std::set<std::wstring> distinct;
            for (size_t r = 0; r < sample; ++r) {
                std::wstring v = Lower(FieldAt(rows[r], static_cast<int>(c)));
                if (!v.empty()) distinct.insert(v);
            }
            if (distinct.size() < fewestDistinct) {
                fewestDistinct = distinct.size();
                catColumn = static_cast<int>(c);
            }
        }
    }

    if (colColumn < 0 || keyColumn < 0) {
        return fail(L"Couldn't find a colour column and a keyword column.", colColumn, keyColumn,
                    catColumn, firstData);
    }

    // --- rows

    std::vector<KeywordRule> rules;
    std::set<std::wstring> seen;
    std::set<std::wstring> reportedDuplicates;

    for (size_t r = firstData; r < rows.size(); ++r) {
        const std::vector<std::wstring>& row = rows[r];
        const std::wstring colorText = FieldAt(row, colColumn);
        const std::wstring keyword = FieldAt(row, keyColumn);
        const std::wstring category = FieldAt(row, catColumn);

        if (colorText.empty() && keyword.empty()) continue;  // spacer

        const std::wstring normalized = Normalize(keyword);
        COLORREF color = kUnmatchedColor;
        const bool colorOk = ParseColorToken(colorText, &color);

        if (normalized.empty() || !colorOk) {
            ++report.skippedRows;
            if (!colorOk && !colorText.empty() && report.badColors.size() < 6) {
                report.badColors.push_back(colorText);
            }
            continue;
        }

        if (seen.count(normalized)) {
            // Kept once; the repeat is reported rather than silently obeyed,
            // because a second rule for the same word can never fire.
            if (reportedDuplicates.insert(normalized).second) report.duplicates.push_back(keyword);
            continue;
        }
        seen.insert(normalized);

        KeywordRule rule;
        rule.category = category.empty() ? std::wstring(kUncategorized) : category;
        rule.colorName = colorText;
        rule.color = color;
        rule.keyword = keyword;
        rule.normalized = normalized;
        rule.wordCount = CountWords(normalized);
        rules.push_back(std::move(rule));
    }

    if (rules.empty()) {
        return fail(L"No usable keyword rows in that file.", colColumn, keyColumn, catColumn,
                    firstData);
    }

    std::set<std::wstring> categorySet;
    for (const KeywordRule& rule : rules) categorySet.insert(rule.category);

    report.ok = true;
    report.rulesImported = static_cast<int>(rules.size());
    report.categories = static_cast<int>(categorySet.size());
    if (out) *out = std::move(rules);
    return report;
}

}  // namespace keywords
}  // namespace rc
