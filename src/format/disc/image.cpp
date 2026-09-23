// Reading a disc image.
//
// Layout facts, from the public hardware documentation (YAGCD) and confirmed
// against dumps:
//
//   0x0000  game id, six characters: system, game, region, maker
//   0x0018  0x5D1C9EA3 on a Wii disc
//   0x001C  0xC2339F3D on a GameCube disc
//   0x0020  the name, up to 0x40 bytes
//   0x0420  where the executable is
//   0x0424  where the file table is, and how long
//
// A GameCube disc's offsets are bytes. A Wii disc's are in units of four bytes,
// because the format predates discs small enough to address directly, and they
// are measured inside a partition rather than on the disc.
#include "wiinx/format/disc/image.hpp"

#include <algorithm>
#include <cstring>

namespace wiinx::disc {
namespace {

constexpr std::uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr std::uint32_t kGameCubeMagic = 0xC2339F3D;
constexpr std::uint64_t kPartitionTableOffset = 0x40000;
constexpr std::size_t kFstEntrySize = 12;

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

struct MemorySource {
    const std::uint8_t* data;
    std::size_t size;
};

bool ReadMemory(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    const auto* source = static_cast<const MemorySource*>(context);
    if (offset + size > source->size) {
        return false;
    }
    std::memcpy(out, source->data + offset, size);
    return true;
}

// A string that stops at its first zero, or at the field's end.
std::string Text(const std::uint8_t* data, std::size_t size) {
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(data, 0, size));
    return std::string(reinterpret_cast<const char*>(data),
                       end != nullptr ? static_cast<std::size_t>(end - data) : size);
}

}  // namespace

Source FromMemory(const std::uint8_t* data, std::size_t size) {
    // The description outlives the call: one per buffer, which is what a test
    // and a small file both want.
    static thread_local MemorySource source;
    source = MemorySource{data, size};
    return Source{&source, &ReadMemory};
}

std::optional<Image> Image::Open(Source source, Cipher cipher) {
    std::uint8_t head[0x460];
    if (!source.Read(0, head, sizeof(head))) {
        return std::nullopt;
    }

    Image image;
    image.mSource = source;
    image.mCipher = cipher;

    const std::uint32_t wii = Read32(head + 0x18);
    const std::uint32_t gamecube = Read32(head + 0x1C);
    if (wii == kWiiMagic) {
        image.mHeader.console = Console::Wii;
    } else if (gamecube == kGameCubeMagic) {
        image.mHeader.console = Console::GameCube;
    } else {
        return std::nullopt;
    }

    image.mHeader.id = Text(head, 6);
    image.mHeader.disc = head[6];
    image.mHeader.version = head[7];
    image.mHeader.name = Text(head + 0x20, 0x40);
    image.mHeader.executable_offset = Read32(head + 0x420);
    image.mHeader.fst_offset = Read32(head + 0x424);
    image.mHeader.fst_size = Read32(head + 0x428);

    if (image.mHeader.console == Console::Wii && !image.OpenGamePartition()) {
        return std::nullopt;
    }
    return image;
}

bool Image::OpenGamePartition() {
    // Four groups of partitions, each an entry count and where that group's
    // entries are. Offsets in the table are in four-byte units.
    std::uint8_t table[0x20];
    if (!mSource.Read(kPartitionTableOffset, table, sizeof(table))) {
        return false;
    }

    for (std::uint32_t group = 0; group < 4; group++) {
        const std::uint32_t count = Read32(table + group * 8);
        const std::uint64_t entries = static_cast<std::uint64_t>(Read32(table + group * 8 + 4)) << 2;
        if (count == 0 || count > 32 || entries == 0) {
            continue;
        }
        for (std::uint32_t index = 0; index < count; index++) {
            std::uint8_t entry[8];
            if (!mSource.Read(entries + index * 8, entry, sizeof(entry))) {
                return false;
            }
            mPartitions.push_back(Partition{group, Read32(entry + 4),
                                            static_cast<std::uint64_t>(Read32(entry)) << 2});
        }
    }

    // The game is in the first partition of type 0; an update partition and a
    // channel installer may sit beside it and are not what a game boots from.
    const Partition* game = nullptr;
    for (const Partition& partition : mPartitions) {
        if (partition.Game()) {
            game = &partition;
            break;
        }
    }
    if (game == nullptr) {
        return false;
    }

    // A partition begins with its ticket, then a header saying where everything
    // else in it is.
    std::uint8_t ticket[0x2C0];
    if (!mSource.Read(game->offset, ticket, sizeof(ticket))) {
        return false;
    }
    const std::uint32_t data_offset_words = Read32(ticket + 0x2B8);
    mDataOffset = game->offset + (static_cast<std::uint64_t>(data_offset_words) << 2);

    // The title key is wrapped with a common key, and the IV is the title id
    // padded with zeros.
    std::memcpy(mTitleKey, ticket + 0x1BF, sizeof(mTitleKey));
    const std::uint8_t key_index = ticket[0x1F1];

    if (mCipher.cbc_decrypt != nullptr && mCipher.common_key != nullptr) {
        const std::uint8_t* common = mCipher.common_key(mCipher.context, key_index);
        if (common != nullptr) {
            std::uint8_t iv[16] = {};
            std::memcpy(iv, ticket + 0x1DC, 8);  // the title id
            mCipher.cbc_decrypt(mCipher.context, common, iv, mTitleKey, mTitleKey,
                                sizeof(mTitleKey));
            mEncrypted = true;
        }
    }

    // A partition's own header repeats the disc header, this time with the
    // offsets a game uses.
    std::uint8_t head[0x460];
    if (!ReadData(0, head, sizeof(head))) {
        return false;
    }
    mHeader.executable_offset = Read32(head + 0x420);
    mHeader.fst_offset = Read32(head + 0x424);
    mHeader.fst_size = Read32(head + 0x428);
    return true;
}

bool Image::ReadCluster(std::uint64_t cluster, std::uint8_t* out) {
    if (mClusterIndex == cluster && mCluster.size() == kClusterDataSize) {
        std::memcpy(out, mCluster.data(), kClusterDataSize);
        return true;
    }

    std::uint8_t raw[kClusterSize];
    if (!mSource.Read(mDataOffset + cluster * kClusterSize, raw, sizeof(raw))) {
        return false;
    }

    if (mEncrypted) {
        // The hash block is encrypted with a zero IV; the data with the IV kept
        // at offset 0x3D0 of that block, which is why the hashes are read first.
        std::uint8_t iv[16] = {};
        mCipher.cbc_decrypt(mCipher.context, mTitleKey, iv, raw, raw, kClusterHashSize);
        std::memcpy(iv, raw + 0x3D0, sizeof(iv));
        mCipher.cbc_decrypt(mCipher.context, mTitleKey, iv, raw + kClusterHashSize, out,
                            kClusterDataSize);
    } else {
        std::memcpy(out, raw + kClusterHashSize, kClusterDataSize);
    }

    // A game reads the same cluster many times over, one file at a time.
    mCluster.assign(out, out + kClusterDataSize);
    mClusterIndex = cluster;
    return true;
}

bool Image::ReadData(std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    if (mHeader.console == Console::GameCube) {
        return mSource.Read(offset, out, size);
    }
    if (mCipher.cbc_decrypt == nullptr && mEncrypted) {
        return false;
    }

    // Reads are cut at cluster boundaries, since each cluster carries its own
    // hashes and its own IV.
    std::uint8_t cluster[kClusterDataSize];
    while (size != 0) {
        const std::uint64_t index = offset / kClusterDataSize;
        const std::size_t within = static_cast<std::size_t>(offset % kClusterDataSize);
        const std::size_t take = std::min(size, kClusterDataSize - within);
        if (!ReadCluster(index, cluster)) {
            return false;
        }
        std::memcpy(out, cluster + within, take);
        out += take;
        offset += take;
        size -= take;
    }
    return true;
}

bool Image::ReadFileTable() {
    mFiles.clear();
    if (mHeader.fst_size == 0 || mHeader.fst_size > 16u * 1024u * 1024u) {
        return false;
    }

    const bool wii = mHeader.console == Console::Wii;
    const std::uint64_t fst = wii ? static_cast<std::uint64_t>(mHeader.fst_offset) << 2
                                  : mHeader.fst_offset;

    std::vector<std::uint8_t> table(mHeader.fst_size);
    if (!ReadData(fst, table.data(), table.size())) {
        return false;
    }
    if (table.size() < kFstEntrySize) {
        return false;
    }

    // The first entry is the root, and its size is how many entries follow it.
    const std::uint32_t count = Read32(table.data() + 8);
    const std::size_t names = count * kFstEntrySize;
    if (count == 0 || names > table.size()) {
        return false;
    }

    const auto name_at = [&](std::uint32_t offset) {
        if (names + offset >= table.size()) {
            return std::string{};
        }
        return Text(table.data() + names + offset, table.size() - names - offset);
    };

    // Folders say which entry they end before, so the path is kept on a stack.
    struct Open {
        std::string path;
        std::uint32_t ends;
    };
    std::vector<Open> open;

    for (std::uint32_t index = 1; index < count; index++) {
        const std::uint8_t* entry = table.data() + index * kFstEntrySize;
        const bool directory = entry[0] != 0;
        const std::uint32_t name_offset = Read32(entry) & 0x00FFFFFF;
        const std::uint32_t second = Read32(entry + 4);
        const std::uint32_t third = Read32(entry + 8);

        while (!open.empty() && index >= open.back().ends) {
            open.pop_back();
        }
        std::string path;
        for (const Open& folder : open) {
            path += folder.path;
            path += '/';
        }
        path += name_at(name_offset);

        if (directory) {
            mFiles.push_back(Entry{path, true, 0, 0});
            open.push_back(Open{name_at(name_offset), third});
        } else {
            const std::uint64_t offset = wii ? static_cast<std::uint64_t>(second) << 2 : second;
            mFiles.push_back(Entry{path, false, offset, third});
        }
    }
    return true;
}

const Entry* Image::Find(const std::string& path) const {
    for (const Entry& entry : mFiles) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> Image::ReadFile(const std::string& path) {
    const Entry* entry = Find(path);
    if (entry == nullptr || entry->directory) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(entry->size);
    if (!bytes.empty() && !ReadData(entry->offset, bytes.data(), bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::vector<std::uint8_t>> Image::ReadExecutable() {
    const std::uint64_t offset = mHeader.console == Console::Wii
                                     ? static_cast<std::uint64_t>(mHeader.executable_offset) << 2
                                     : mHeader.executable_offset;

    // A DOL says how long it is only by where its sections end, so its size is
    // the largest of those.
    std::uint8_t head[0x100];
    if (!ReadData(offset, head, sizeof(head))) {
        return std::nullopt;
    }
    std::uint32_t end = 0;
    for (int section = 0; section < 18; section++) {
        const std::uint32_t at = Read32(head + section * 4);
        const std::uint32_t size = Read32(head + 0x90 + section * 4);
        if (at != 0 && at + size > end) {
            end = at + size;
        }
    }
    if (end == 0 || end > 64u * 1024u * 1024u) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(end);
    if (!ReadData(offset, bytes.data(), bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace wiinx::disc
