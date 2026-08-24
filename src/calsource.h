// calsource.h — turning whatever the user pasted into a feed URL.
//
// Pure string work, no network. People paste Google embed links, "add by URL"
// links, webcal:// links, a bare calendar address, or a path to a local file,
// and all of them should just work.

#pragma once

#include <optional>
#include <string>

#include "common.h"

namespace rc {
namespace calsource {

// Normalises `input` to a fetchable URL, or nothing if it cannot be read as a
// calendar link at all. Applied in this exact order:
//
//   1. trim; empty -> none
//   2. webcal:// -> https://
//   3. already ends .ics -> as-is  (this is also the only way a file:// path
//      survives: a local feed must end in .ics)
//   4. has a src= query parameter -> derive the Google ical URL. One rule
//      covers both /calendar/embed?src= and /calendar/u/0/newembed?src=
//   5. bare address (no ://, no /, contains @) -> derive the same way
//   6. starts with http -> verbatim
//   7. otherwise none
std::optional<std::wstring> ToIcs(const std::wstring& input);

// The `ctz=` parameter of the *original* input, not the derived URL. This is
// the display time zone: a shared calendar published in another zone should
// read in that zone, not the reader's.
std::optional<std::wstring> TimeZoneOf(const std::wstring& input);

// A human label for a link: the src value, else a bare address, else the
// segment between /calendar/ical/ and the next slash, else the input.
std::wstring Label(const std::wstring& input);

// Empty when the link is usable; otherwise the message to show the user.
std::wstring Problem(const std::wstring& input);

bool IsFileUrl(const std::wstring& url);
std::wstring FileUrlToPath(const std::wstring& url);

}  // namespace calsource
}  // namespace rc
