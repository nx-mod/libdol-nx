// Installing a title into a NAND, listing it, and taking it out again - with
// the NAND kept in memory, which is all a store has to be.
#include "wiinx/format/nand/store.hpp"
#include "wiinx/format/nand/wad.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

// A NAND that is a map of paths to bytes.
struct Memory {
    std::map<std::string, std::vector<std::uint8_t>> files;

    static bool Read(void* context, const std::string& path, std::vector<std::uint8_t>& out) {
        auto* self = static_cast<Memory*>(context);
        const auto found = self->files.find(path);
        if (found == self->files.end()) {
            return false;
        }
        out = found->second;
        return true;
    }

    static bool Write(void* context, const std::string& path, const std::uint8_t* data,
                      std::size_t size) {
        auto* self = static_cast<Memory*>(context);
        self->files[path].assign(data, data + size);
        return true;
    }

    static bool Remove(void* context, const std::string& path) {
        return static_cast<Memory*>(context)->files.erase(path) != 0;
    }

    static bool List(void* context, const std::string& path, std::vector<std::string>& out) {
        auto* self = static_cast<Memory*>(context);
        const std::string prefix = path + "/";
        std::set<std::string> names;
        for (const auto& [name, _] : self->files) {
            if (name.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string rest = name.substr(prefix.size());
            names.insert(rest.substr(0, rest.find('/')));
        }
        out.assign(names.begin(), names.end());
        return !out.empty();
    }

    wiinx::nand::Store AsStore() { return {this, &Read, &Write, &Remove, &List}; }
};

// A stand-in cipher: it flips every byte, which is reversible and is not the
// identity. What is checked is that the right bytes reach it with the right
// key, not AES.
void Flip(void*, const std::uint8_t[16], const std::uint8_t[16], const std::uint8_t* in,
          std::uint8_t* out, std::size_t size) {
    for (std::size_t index = 0; index < size; index++) {
        out[index] = static_cast<std::uint8_t>(~in[index]);
    }
}

const std::uint8_t* CommonKey(void*, std::uint8_t index) {
    static const std::uint8_t key[16] = {};
    return index <= 2 ? key : nullptr;
}

void Put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 8);
    out[at + 1] = static_cast<std::uint8_t>(value);
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int index = 0; index < 4; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (24 - index * 8));
    }
}

void Put64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
    for (int index = 0; index < 8; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (56 - index * 8));
    }
}

constexpr std::size_t kSignature = 0x100 + 0x3C;

// A title: one content of its own, and one it shares.
std::vector<std::uint8_t> MakeWad(std::uint64_t title_id, const std::string& own,
                                  const std::string& shared) {
    using namespace wiinx::nand;

    std::vector<std::uint8_t> ticket(kSignature + Ticket::kBodySize, 0);
    Put32(ticket, 0, 0x00010001);
    const char* issuer = "Root-CA00000001-XS00000003";
    std::memcpy(ticket.data() + kSignature, issuer, std::strlen(issuer));
    Put64(ticket, kSignature + 0x9C, title_id);

    std::vector<std::uint8_t> tmd(kSignature + 0xA4 + 2 * 0x24, 0);
    Put32(tmd, 0, 0x00010001);
    const char* signer = "Root-CA00000001-CP00000004";
    std::memcpy(tmd.data() + kSignature, signer, std::strlen(signer));
    Put64(tmd, kSignature + 0x4C, title_id);
    Put16(tmd, kSignature + 0x9C, 3);   // version
    Put16(tmd, kSignature + 0x9E, 2);   // two contents

    Put32(tmd, kSignature + 0xA4, 0x00000001);          // its own: content id 1
    Put16(tmd, kSignature + 0xA8, 0);                   // index
    Put16(tmd, kSignature + 0xAA, 1);                   // an ordinary content
    Put64(tmd, kSignature + 0xAC, own.size());

    Put32(tmd, kSignature + 0xC8, 0x00000002);          // the shared one
    Put16(tmd, kSignature + 0xCC, 1);
    Put16(tmd, kSignature + 0xCE, 0x8001);              // shared
    Put64(tmd, kSignature + 0xD0, shared.size());
    // Its hash is what the map matches on; any distinct value will do here.
    tmd[kSignature + 0xD8] = 0xAB;
    tmd[kSignature + 0xD9] = 0xCD;

    const auto encrypt = [](const std::string& text) {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.resize((bytes.size() + 15) & ~std::size_t{15}, 0);
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(~byte);
        }
        return bytes;
    };

    return Wad::Build(WadKind::Installable, std::vector<std::uint8_t>(0x780, 0x11), ticket, tmd,
                      {encrypt(own), encrypt(shared)});
}
}  // namespace

int main() {
    using namespace wiinx::nand;
    std::printf("wiinx::nand store check\n\n");

    Memory memory;
    const Store store = memory.AsStore();
    Cipher cipher;
    cipher.cbc_decrypt = &Flip;
    cipher.common_key = &CommonKey;

    constexpr std::uint64_t kFirst = 0x0001000157523945ull;   // 00010001/WR9E
    constexpr std::uint64_t kSecond = 0x0001000157525845ull;  // 00010001/WRXE

    Check("nothing is installed to begin with", InstalledTitles(store).empty());

    const auto wad = MakeWad(kFirst, "the game itself", "the shared library");
    Check("a WAD installs", InstallTitle(store, wad, cipher) == InstallResult::Installed);

    const TitleId first{kFirst};
    Check("its ticket is where a console keeps one",
          memory.files.count(TicketPath(first)) == 1);
    Check("its TMD likewise", memory.files.count(TmdPath(first)) == 1);

    const auto content = memory.files.find(ContentFilePath(first, 1));
    Check("its own content is decrypted", content != memory.files.end());
    if (content != memory.files.end()) {
        Check("  and is what went in",
              std::string(content->second.begin(), content->second.end()) == "the game itself");
    }

    Check("the shared content went to /shared1",
          memory.files.count("/shared1/00000000.app") == 1);
    Check("and the map says so", memory.files.count("/shared1/content.map") == 1);
    Check("a shared content is not in the title's own folder",
          memory.files.count(ContentFilePath(first, 2)) == 0);

    Check("it is listed", InstalledTitles(store).size() == 1);
    Check("by id and version", InstalledTitles(store)[0].id == first &&
                                   InstalledTitles(store)[0].version == 3);
    Check("and asked about directly", TitleInstalled(store, first));
    Check("one that is not there is not claimed", !TitleInstalled(store, TitleId{kSecond}));

    // A second title sharing the same content writes no second copy.
    const auto second = MakeWad(kSecond, "another game", "the shared library");
    Check("a second title installs", InstallTitle(store, second, cipher) == InstallResult::Installed);
    Check("  and both are listed", InstalledTitles(store).size() == 2);
    Check("  the shared content is stored once",
          memory.files.count("/shared1/00000001.app") == 0);
    Check("  and the map still has one entry",
          memory.files["/shared1/content.map"].size() == 28);

    // Removing one leaves the other, and leaves what they share.
    Check("a title is removed", RemoveTitle(store, first));
    Check("  its content is gone", memory.files.count(ContentFilePath(first, 1)) == 0);
    Check("  its ticket is gone", memory.files.count(TicketPath(first)) == 0);
    Check("  the shared content stays", memory.files.count("/shared1/00000000.app") == 1);
    Check("  and the other title is still there", TitleInstalled(store, TitleId{kSecond}));

    // Without a key, nothing is written rather than something wrong.
    Memory empty;
    Check("a WAD with no key to open it is refused",
          InstallTitle(empty.AsStore(), wad, Cipher{}) == InstallResult::NoKey);
    Check("  and nothing was written", empty.files.empty());

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
