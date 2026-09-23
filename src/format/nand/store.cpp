// Installing a title into a NAND, and reading back what is there.
//
// A console keeps a title as three things: its ticket at /ticket/<high>/<low>,
// its TMD and contents under /title/<high>/<low>/content, and - for the
// contents it shares with other titles - a copy in /shared1 with an entry in
// the map beside them. Contents are kept decrypted: what encrypts a real NAND
// is the filesystem underneath, not the title.
#include "wiinx/format/nand/store.hpp"

#include "wiinx/format/nand/wad.hpp"

#include <cstdio>
#include <cstring>

namespace wiinx::nand {
namespace {

constexpr std::string_view kSharedPath = "/shared1";
constexpr std::size_t kSharedEntrySize = 28;  // a SHA-1, then eight characters

std::string Hex8(std::uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

// /shared1/content.map: one entry per shared content, its hash and the name it
// was given. A title finds a content it shares by looking its hash up here.
struct SharedMap {
    std::vector<std::uint8_t> bytes;

    std::size_t count() const { return bytes.size() / kSharedEntrySize; }

    // The name a hash already has, or nothing when it is not here yet.
    std::optional<std::string> find(const std::array<std::uint8_t, 20>& sha1) const {
        for (std::size_t index = 0; index < count(); index++) {
            const std::uint8_t* entry = bytes.data() + index * kSharedEntrySize;
            if (std::memcmp(entry, sha1.data(), sha1.size()) == 0) {
                return std::string(reinterpret_cast<const char*>(entry + 20), 8);
            }
        }
        return std::nullopt;
    }

    // Adds one, named the way the console names them: in order, from 00000000.
    std::string add(const std::array<std::uint8_t, 20>& sha1) {
        const std::string name = Hex8(static_cast<std::uint32_t>(count()));
        bytes.insert(bytes.end(), sha1.begin(), sha1.end());
        bytes.insert(bytes.end(), name.begin(), name.end());
        return name;
    }
};

}  // namespace

std::vector<InstalledTitle> InstalledTitles(const Store& store) {
    std::vector<InstalledTitle> found;
    if (store.list == nullptr) {
        return found;
    }

    std::vector<std::string> highs;
    if (!store.list(store.context, "/title", highs)) {
        return found;
    }
    for (const std::string& high : highs) {
        std::vector<std::string> lows;
        if (!store.list(store.context, "/title/" + high, lows)) {
            continue;
        }
        for (const std::string& low : lows) {
            const TitleId id{(std::strtoull(high.c_str(), nullptr, 16) << 32) |
                             std::strtoul(low.c_str(), nullptr, 16)};

            std::vector<std::uint8_t> bytes;
            if (!store.Read(TmdPath(id), bytes)) {
                continue;
            }
            const auto tmd = Tmd::Parse(bytes.data(), bytes.size());
            if (!tmd) {
                continue;
            }
            found.push_back(InstalledTitle{id, tmd->title_version,
                                           static_cast<std::uint16_t>(tmd->contents.size()),
                                           tmd->TotalSize()});
        }
    }
    return found;
}

bool TitleInstalled(const Store& store, TitleId id) {
    std::vector<std::uint8_t> bytes;
    return store.Read(TmdPath(id), bytes) && Tmd::Parse(bytes.data(), bytes.size()).has_value();
}

InstallResult InstallTitle(const Store& store, const std::vector<std::uint8_t>& file,
                           const Cipher& cipher) {
    const auto wad = Wad::Parse(file);
    if (!wad) {
        return InstallResult::NotAWad;
    }
    if (cipher.cbc_decrypt == nullptr || cipher.common_key == nullptr) {
        return InstallResult::NoKey;
    }

    // The title key is wrapped with a common key the ticket names.
    const Ticket& ticket = wad->GetTicket();
    const std::uint8_t* common = cipher.common_key(cipher.context, ticket.common_key_index);
    if (common == nullptr) {
        return InstallResult::NoKey;
    }
    std::uint8_t title_key[16];
    std::memcpy(title_key, ticket.title_key.data(), sizeof(title_key));
    const auto key_iv = TitleKeyIv(ticket.title_id);
    cipher.cbc_decrypt(cipher.context, common, key_iv.data(), title_key, title_key,
                       sizeof(title_key));

    const Tmd& tmd = wad->GetTmd();
    const TitleId id{tmd.title_id};

    if (!store.Write(TicketPath(id), ticket.raw) || !store.Write(TmdPath(id), tmd.raw)) {
        return InstallResult::WriteFailed;
    }

    // The shared map, as it stands. A title that shares a content another one
    // already installed writes nothing but the map entry.
    SharedMap shared;
    store.Read(std::string(kSharedPath) + "/content.map", shared.bytes);

    for (const ContentRecord& content : tmd.contents) {
        const auto extent = wad->Content(content.index);
        if (!extent) {
            return InstallResult::WriteFailed;
        }

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(content.EncryptedSize()));
        std::memcpy(bytes.data(), file.data() + extent->offset, bytes.size());

        const auto iv = ContentIv(content.index);
        cipher.cbc_decrypt(cipher.context, title_key, iv.data(), bytes.data(), bytes.data(),
                           bytes.size());
        bytes.resize(static_cast<std::size_t>(content.size));

        if (content.Shared()) {
            // Shared contents live once, under a name the map gives them.
            std::string name;
            if (const auto known = shared.find(content.sha1)) {
                name = *known;
            } else {
                name = shared.add(content.sha1);
                if (!store.Write(std::string(kSharedPath) + "/" + name + ".app", bytes)) {
                    return InstallResult::WriteFailed;
                }
            }
            continue;
        }

        if (!store.Write(ContentFilePath(id, content.id), bytes)) {
            return InstallResult::WriteFailed;
        }
    }

    if (!shared.bytes.empty() &&
        !store.Write(std::string(kSharedPath) + "/content.map", shared.bytes)) {
        return InstallResult::WriteFailed;
    }
    return InstallResult::Installed;
}

bool RemoveTitle(const Store& store, TitleId id) {
    if (store.remove == nullptr || store.list == nullptr) {
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!store.Read(TmdPath(id), bytes)) {
        return false;
    }
    const auto tmd = Tmd::Parse(bytes.data(), bytes.size());
    if (!tmd) {
        return false;
    }

    // Only what this title owns. A shared content belongs to whoever else is
    // using it, and the map is what says so.
    for (const ContentRecord& content : tmd->contents) {
        if (!content.Shared()) {
            store.remove(store.context, ContentFilePath(id, content.id));
        }
    }
    store.remove(store.context, TmdPath(id));
    store.remove(store.context, TicketPath(id));
    return true;
}

}  // namespace wiinx::nand
