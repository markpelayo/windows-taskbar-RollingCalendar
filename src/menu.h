// menu.h — the right-click menu, rebuilt from scratch every time it opens.
//
// Rebuilt rather than kept live because almost every row's title depends on
// state that changes between openings: the day's blocks, the freshness caption,
// whether Sound Hours currently permits noise. Keeping a live menu in sync with
// all of that costs more code than rebuilding it, and gets it wrong more often.
//
// The day's blocks are owner-drawn, because they are five aligned columns with
// an inline colour swatch and HMENU text has neither tab stops nor swatches.
// Everything else is an ordinary menu item.

#pragma once

#include <vector>

#include "common.h"
#include "daylist.h"
#include "raii.h"

namespace rc {

class App;

namespace menu {

// Builds the whole menu for the current state. The caller owns the returned
// HMENU. `rows` is retained by the menu for owner-drawing and must outlive it.
UniqueMenu Build(App& app, const std::vector<DayRow>& rows);

// Owner-draw plumbing for the day rows and the caption lines.
void OnMeasureItem(HWND hwnd, MEASUREITEMSTRUCT* mis);
void OnDrawItem(HWND hwnd, DRAWITEMSTRUCT* dis);

// Runs a command id. Returns false if the id was not one of ours.
bool Invoke(App& app, HWND owner, UINT id);

// Frees the owner-draw payloads after the menu closes.
void ReleaseItemData();

}  // namespace menu
}  // namespace rc
