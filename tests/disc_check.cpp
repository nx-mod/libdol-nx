// Disc images: built here, read back through the library.
//
// Two discs are assembled in memory - a GameCube one, and a Wii one with a
// partition, clusters and a stand-in cipher - so the layout logic is checked on
// any machine, with nothing Nintendo made involved.
#include "wiinx/format/disc/image.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int index = 0; index < 4; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (24 - index * 8));
    }
}

void PutText(std::vector<std::uint8_t>& out, std::size_t at, const std::string& text) {
    std::memcpy(out.data() + at, text.data(), text.size());
}

// One file the test disc carries.
struct File {
    std::string name;
    std::string contents;
};

// A file table: the root, one entry per file, then the names.
std::vector<std::uint8_t> BuildFst(const std::vector<File>& files, std::uint32_t data_offset,
                                   bool wii, std::vector<std::uint32_t>& offsets) {
    const std::uint32_t count = static_cast<std::uint32_t>(files.size()) + 1;
    std::vector<std::uint8_t> names;
    std::vector<std::uint32_t> name_offsets;
    for (const File& file : files) {
        name_offsets.push_back(static_cast<std::uint32_t>(names.size()));
        names.insert(names.end(), file.name.begin(), file.name.end());
        names.push_back(0);
    }

    std::vector<std::uint8_t> fst(count * 12 + names.size(), 0);
    fst[0] = 1;                       // the root is a directory
    Put32(fst, 8, count);             // ...and its size is how many entries follow

    std::uint32_t at = data_offset;
    for (std::size_t index = 0; index < files.size(); index++) {
        const std::size_t entry = (index + 1) * 12;
        Put32(fst, entry, name_offsets[index]);
        Put32(fst, entry + 4, wii ? at >> 2 : at);
        Put32(fst, entry + 8, static_cast<std::uint32_t>(files[index].contents.size()));
        offsets.push_back(at);
        at += static_cast<std::uint32_t>((files[index].contents.size() + 31) & ~31u);
    }
    std::memcpy(fst.data() + count * 12, names.data(), names.size());
    return fst;
}

std::vector<std::uint8_t> BuildGameCubeDisc(const std::vector<File>& files) {
    constexpr std::uint32_t kFstAt = 0x1000;
    constexpr std::uint32_t kDataAt = 0x4000;

    std::vector<std::uint32_t> offsets;
    const std::vector<std::uint8_t> fst = BuildFst(files, kDataAt, false, offsets);

    std::vector<std::uint8_t> disc(0x10000, 0);
    PutText(disc, 0, "GM4E01");
    Put32(disc, 0x1C, 0xC2339F3D);
    PutText(disc, 0x20, "TEST DISC");
    Put32(disc, 0x420, 0x2000);   // the executable
    Put32(disc, 0x424, kFstAt);
    Put32(disc, 0x428, static_cast<std::uint32_t>(fst.size()));
    std::memcpy(disc.data() + kFstAt, fst.data(), fst.size());
    for (std::size_t index = 0; index < files.size(); index++) {
        std::memcpy(disc.data() + offsets[index], files[index].contents.data(),
                    files[index].contents.size());
    }
    return disc;
}

// A stand-in cipher: it reverses each byte's bits, which is reversible, is not
// the identity, and needs no key material in the repository. What is checked is
// that the right bytes are handed to it with the right key and IV, not AES.
void FlipBytes(void*, const std::uint8_t key[16], const std::uint8_t iv[16],
               const std::uint8_t* in, std::uint8_t* out, std::size_t size) {
    (void)key;
    (void)iv;
    for (std::size_t index = 0; index < size; index++) {
        out[index] = static_cast<std::uint8_t>(~in[index]);
    }
}

const std::uint8_t* CommonKey(void*, std::uint8_t) {
    static const std::uint8_t key[16] = {};
    return key;
}

std::vector<std::uint8_t> BuildWiiDisc(const std::vector<File>& files, bool encrypted) {
    using namespace wiinx::disc;
    constexpr std::uint64_t kPartitionAt = 0x50000;
    constexpr std::uint32_t kPartitionDataAt = 0x8000;   // within the partition
    constexpr std::uint32_t kFstAt = 0x1000;             // within the partition's data
    constexpr std::uint32_t kDataAt = 0x8000;

    std::vector<std::uint32_t> offsets;
    const std::vector<std::uint8_t> fst = BuildFst(files, kDataAt, true, offsets);

    // The partition's own data, before it is cut into clusters.
    std::vector<std::uint8_t> data(0x20000, 0);
    PutText(data, 0, "RMCE01");
    Put32(data, 0x18, 0x5D1C9EA3);
    PutText(data, 0x20, "TEST PARTITION");
    Put32(data, 0x420, 0x2000 >> 2);
    Put32(data, 0x424, kFstAt >> 2);
    Put32(data, 0x428, static_cast<std::uint32_t>(fst.size()));
    std::memcpy(data.data() + kFstAt, fst.data(), fst.size());
    for (std::size_t index = 0; index < files.size(); index++) {
        std::memcpy(data.data() + offsets[index], files[index].contents.data(),
                    files[index].contents.size());
    }

    std::vector<std::uint8_t> disc(0x200000, 0);
    PutText(disc, 0, "RMCE01");
    Put32(disc, 0x18, 0x5D1C9EA3);
    PutText(disc, 0x20, "TEST DISC");

    // One partition, in the first group.
    Put32(disc, 0x40000, 1);
    Put32(disc, 0x40004, 0x40020 >> 2);
    Put32(disc, 0x40020, static_cast<std::uint32_t>(kPartitionAt >> 2));
    Put32(disc, 0x40024, 0);  // a game partition

    // Its ticket says where the data begins; the key is whatever the cipher
    // makes of these bytes.
    Put32(disc, static_cast<std::size_t>(kPartitionAt) + 0x2B8, kPartitionDataAt >> 2);

    // The data, as clusters: a hash block, then 0x7C00 of payload.
    const std::uint64_t data_at = kPartitionAt + kPartitionDataAt;
    for (std::size_t cluster = 0; cluster * kClusterDataSize < data.size(); cluster++) {
        const std::size_t at = static_cast<std::size_t>(data_at) + cluster * kClusterSize;
        const std::size_t take =
            std::min(kClusterDataSize, data.size() - cluster * kClusterDataSize);
        std::uint8_t* payload = disc.data() + at + kClusterHashSize;
        std::memcpy(payload, data.data() + cluster * kClusterDataSize, take);
        if (encrypted) {
            // Stored the way the cipher will undo it.
            for (std::size_t index = 0; index < kClusterDataSize; index++) {
                payload[index] = static_cast<std::uint8_t>(~payload[index]);
            }
        }
    }
    return disc;
}
}  // namespace

int main() {
    using namespace wiinx::disc;
    std::printf("wiinx::disc check\n\n");

    const std::vector<File> files{
        {"boot.bin", "the first file"},
        {"course.szs", std::string(5000, 'C')},
        {"sound.brsar", "music"},
    };

    {   // A GameCube disc.
        const auto bytes = BuildGameCubeDisc(files);
        auto image = Image::Open(FromMemory(bytes.data(), bytes.size()));
        Check("GameCube disc opens", image.has_value());
        if (image) {
            Check("  console", image->GetConsole() == Console::GameCube);
            Check("  id", image->GetHeader().id == "GM4E01");
            Check("  name", image->GetHeader().name == "TEST DISC");
            Check("  has no partitions", image->Partitions().empty());
            Check("  file table", image->ReadFileTable() && image->Files().size() == files.size());
            const auto course = image->ReadFile("course.szs");
            Check("  a file reads back",
                  course && course->size() == 5000 && (*course)[0] == 'C');
            Check("  an unknown file is not found", !image->ReadFile("nothing").has_value());
        }
    }

    {   // A Wii disc, unencrypted: the partition and cluster layout alone.
        const auto bytes = BuildWiiDisc(files, false);
        auto image = Image::Open(FromMemory(bytes.data(), bytes.size()));
        Check("Wii disc opens", image.has_value());
        if (image) {
            Check("  console", image->GetConsole() == Console::Wii);
            Check("  one game partition",
                  image->Partitions().size() == 1 && image->Partitions()[0].Game());
            Check("  file table", image->ReadFileTable() && image->Files().size() == files.size());
            const auto course = image->ReadFile("course.szs");
            // 5000 bytes crosses no cluster boundary; boot.bin and this one do
            // land in the same cluster, which is what the cache is for.
            Check("  a file reads back", course && course->size() == 5000 && (*course)[4999] == 'C');
            const auto first = image->ReadFile("boot.bin");
            Check("  a second file from the same cluster",
                  first && std::string(first->begin(), first->end()) == "the first file");
        }
    }

    {   // A Wii disc whose clusters need the caller's cipher.
        const auto bytes = BuildWiiDisc(files, true);
        Cipher cipher;
        cipher.cbc_decrypt = &FlipBytes;
        cipher.common_key = &CommonKey;
        auto image = Image::Open(FromMemory(bytes.data(), bytes.size()), cipher);
        Check("an encrypted Wii disc opens", image.has_value());
        if (image) {
            Check("  file table", image->ReadFileTable() && image->Files().size() == files.size());
            const auto sound = image->ReadFile("sound.brsar");
            Check("  a file reads back through the cipher",
                  sound && std::string(sound->begin(), sound->end()) == "music");
        }
    }

    {   // Something that is not a disc at all.
        const std::vector<std::uint8_t> junk(0x1000, 0x5A);
        Check("a buffer that is not a disc is refused",
              !Image::Open(FromMemory(junk.data(), junk.size())).has_value());
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
