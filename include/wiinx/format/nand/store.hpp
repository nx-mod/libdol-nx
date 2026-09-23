#pragma once

// A NAND, as somewhere files can be kept.
//
// The console's internal memory is a tree of files, and every consumer of it
// keeps that tree somewhere different: the runtime on an SD card, a tool in a
// folder on a PC, a test in memory. So this section does no I/O at all - a
// caller hands over four functions, and the formats are read and written
// through them.
//
// What that buys: installing a title, listing what is installed and removing
// one are written once, and work the same wherever the NAND lives.

#include "wiinx/format/nand/isfs.hpp"
#include "wiinx/format/nand/title.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::nand {

// Somewhere a NAND's files are kept. Paths are the ones in isfs.hpp: absolute,
// '/'-separated, whatever the host's own separator is.
struct Store {
    void* context = nullptr;

    bool (*read)(void* context, const std::string& path, std::vector<std::uint8_t>& out) = nullptr;
    bool (*write)(void* context, const std::string& path, const std::uint8_t* data,
                  std::size_t size) = nullptr;
    bool (*remove)(void* context, const std::string& path) = nullptr;
    // Every name directly inside `path`, files and folders alike.
    bool (*list)(void* context, const std::string& path, std::vector<std::string>& out) = nullptr;

    bool Read(const std::string& path, std::vector<std::uint8_t>& out) const {
        return read != nullptr && read(context, path, out);
    }
    bool Write(const std::string& path, const std::vector<std::uint8_t>& bytes) const {
        return write != nullptr && write(context, path, bytes.data(), bytes.size());
    }
};

// AES-128-CBC, which a title's contents are encrypted with. The keys are the
// caller's: none is in this library.
struct Cipher {
    void* context = nullptr;
    void (*cbc_decrypt)(void* context, const std::uint8_t key[16], const std::uint8_t iv[16],
                        const std::uint8_t* in, std::uint8_t* out, std::size_t size) = nullptr;
    const std::uint8_t* (*common_key)(void* context, std::uint8_t index) = nullptr;
};

// One title in a NAND, as a listing shows it.
struct InstalledTitle {
    TitleId id;
    std::uint16_t version = 0;
    std::uint16_t contents = 0;
    std::uint64_t size = 0;  // what its contents take, decrypted
};

// Everything installed, in the order the NAND lists it.
std::vector<InstalledTitle> InstalledTitles(const Store& store);

// Whether a title is there at all, which is what a launcher asks first.
bool TitleInstalled(const Store& store, TitleId id);

enum class InstallResult {
    Installed,
    NotAWad,
    NoKey,          // the ticket names a common key the caller did not supply
    WriteFailed,
};

// Installs a WAD: its ticket, its TMD, and every content decrypted into the
// place the console keeps it. Contents a title shares with others go to
// `/shared1`, and the map there is updated so another title finds them.
//
// Nothing is verified. A console checks signatures when it launches a title,
// not when it writes one, and a caller that wants more than that has the
// ticket and the TMD to look at.
InstallResult InstallTitle(const Store& store, const std::vector<std::uint8_t>& wad,
                           const Cipher& cipher);

// Removes a title's own files. Shared contents stay: another title may be
// using them, and the map says who.
bool RemoveTitle(const Store& store, TitleId id);

}  // namespace wiinx::nand
