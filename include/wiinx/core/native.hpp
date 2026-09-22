// Natives: a library function's name, the library version it matches, and
// the code that replaces it.
//
// Each module lists its natives in one plain table and core joins the
// tables - no self-registering statics, so nothing is silently dropped when
// the module is linked as a static library.
//
//     const Native kLytNatives[] = {
//         WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kNw4rLyt_2008_03, pane_calculate_mtx_2008),
//     };
#pragma once

#include "wiinx/core/types.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace wiinx {

struct Cpu;

// One build of one library, as the game itself names it: the version string
// Nintendo's libraries register at startup, e.g.
//     << NW4R    - LYT    final   build: Mar  8 2008 20:59:41 (0x4201_127) >>
struct LibVersion {
    std::string_view id;     // stable id used in bindings: "nw4r.lyt@2008-03-08"
    std::string_view lib;    // the name in the banner: "NW4R - LYT"
    std::string_view build;  // the banner's date and code: "Mar  8 2008 (0x4201_127)"
};

using NativeFn = void (*)(Cpu*);

struct Native {
    std::string_view name;       // the original's qualified name
    const LibVersion* version;   // which build of the library it matches
    NativeFn entry;
};

#define WIINX_NATIVE(name, version, fn) ::wiinx::Native{name, &(version), fn}

// Every native in every linked module.
std::span<const Native> natives() noexcept;

// The native for `name` in `version_id`, or null when none is known.
const Native* find_native(std::string_view name, std::string_view version_id) noexcept;

}  // namespace wiinx
