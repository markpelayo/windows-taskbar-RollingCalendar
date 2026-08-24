// keywords.h — where the colour comes from.
//
// iCalendar feeds carry no colour of their own, so a block's colour is decided
// by matching keyword rules against its title. Matching is whole-word, so
// "meal" matches "Prep the meal" but not "Oatmeal", and the longest phrase
// wins, so "meal prep" beats "meal" when both could match.
//
// The CSV importer is deliberately forgiving: people export these from
// spreadsheets, and a file that fails to import because of a semicolon
// delimiter or a missing header row helps nobody.

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "ics.h"

namespace rc {

struct KeywordRule {
    std::wstring category;    // "Focus Work | Learn"
    std::wstring colorName;   // as written in the CSV, may be empty
    COLORREF color = RGB(0x8E, 0x8E, 0x93);
    std::wstring keyword;     // as written
    std::wstring normalized;  // Normalize(keyword), matched against
    int wordCount = 1;
};

struct CsvImportReport {
    bool ok = false;
    std::wstring error;               // set when ok == false
    std::wstring diagnostics;         // delimiter, columns, first data row
    int rulesImported = 0;
    int categories = 0;
    int skippedRows = 0;
    std::vector<std::wstring> badColors;    // first 6
    std::vector<std::wstring> duplicates;   // repeated keywords, kept once
};

namespace keywords {

// The rules currently in force, pre-sorted so the first match is the right one:
// word count descending, then normalized length descending, then file order.
const std::vector<KeywordRule>& Rules();
void SetRules(std::vector<KeywordRule> rules, const std::wstring& sourceName);
void Clear();

// Human description of where the rules came from ("the built-in sample", a
// file name). Shown in the menu.
const std::wstring& SourceName();

// The 42-rule, 6-category sample. Seeded once on first launch and guarded by a
// flag, so cleared stays cleared.
std::vector<KeywordRule> SampleRules();
std::wstring SampleCsv();

// Sets `color` and `category` on every event whose title matches a rule.
// Unmatched events are left alone and draw in the unmatched colour.
void Apply(std::vector<CalEvent>& events);

// The distinct categories in the current rule set, in first-seen order, with
// how many rules each carries. Drives the Keyword Colors menu.
struct CategorySummary {
    std::wstring name;
    COLORREF color;
    int ruleCount;
};
std::vector<CategorySummary> Categories();

// Colour for a category name, or the unmatched grey.
COLORREF ColorForCategory(const std::wstring& category);

// Named colours: red, orange, yellow, green, mint, teal, cyan/aqua, blue,
// indigo, purple/violet, pink/magenta/fuchsia, brown, gray/grey, light gray,
// dark gray/charcoal, black, white. Falls through to ParseHexColor.
bool ParseColorToken(const std::wstring& token, COLORREF* out);

// Parses CSV text into rules. Handles CRLF/CR, a BOM, quoted fields with
// doubled quotes, auto-detects the delimiter from the first four lines, and
// identifies the colour / keyword / category columns by header name -- or, when
// there is no header row, by inspecting the first twenty rows.
CsvImportReport ParseCsv(const std::wstring& text, std::vector<KeywordRule>* out);

extern const COLORREF kUnmatchedColor;          // #8E8E93
extern const wchar_t* const kUncategorized;     // L"Uncategorized"

}  // namespace keywords
}  // namespace rc
