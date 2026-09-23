// Reading an RVZ or WIA file.
//
// Structure, from Dolphin's docs/WiaAndRvz.md. Every field is big-endian.
//
//   0x00  the file header: magic, versions, the size and hash of what follows
//   0x48  the disc: what kind, how it was compressed, the chunk size, and where
//         the three tables are
//   then  the partition table (plain), the raw-data table and the group table
//         (both compressed with the file's own method)
//
// The disc is described as regions. A raw-data region is the disc as it is - the
// header, the partition table, anything outside a partition. A partition region
// is a Wii partition's contents, stored decrypted and with the hash block of
// each cluster removed, so 0x7C00 bytes stand for every 0x8000 on the disc.
//
// Each region names a run of groups, and a group holds one chunk of it.
//
// What this hands out is therefore not quite the disc: raw-data regions sit at
// their real offsets, and a partition's data is a decrypted stream beginning
// where that partition's data begins. That is exactly what a reader wants -
// `Image` is told the partitions are already plain and stops looking for a
// cipher.
#include "wiinx/format/disc/rvz.hpp"

#include <algorithm>
#include <cstring>

namespace wiinx::disc {
namespace {

constexpr std::size_t kFileHeaderSize = 0x48;
constexpr std::size_t kDiscSize = 0xDC;
constexpr std::size_t kPartitionEntrySize = 48;
constexpr std::size_t kRawDataEntrySize = 24;
constexpr std::size_t kClusterStored = kClusterDataSize;  // 0x7C00 per 0x8000
// A group of clusters: what one hash-exception list covers.
constexpr std::size_t kGroupData = 64 * kClusterStored;  // 0x1F0000
// The most one list can take, so a chunk that carries lists is decompressed
// into a buffer with room for them.
constexpr std::size_t kListSlack = 0x12000;
constexpr std::size_t kExceptionEntry = 2 + 20;  // where a hash goes, and the hash

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t Read64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(Read32(p)) << 32) | Read32(p + 4);
}

}  // namespace

// The lists of hashes a chunk of a partition carries in front of its data:
// each is a count and that many (where, hash) pairs. Nothing here checks a
// hash, so they are measured and stepped over. When they were stored without
// compression the last one is padded so the data behind it starts aligned.
std::size_t SkipExceptionLists(const std::uint8_t* data, std::size_t size, std::uint32_t lists,
                               bool aligned) {
    std::size_t at = 0;
    for (std::uint32_t list = 0; list < lists; list++) {
        if (at + 2 > size) {
            return at;
        }
        const std::size_t count = (static_cast<std::size_t>(data[at]) << 8) | data[at + 1];
        std::size_t length = 2 + count * kExceptionEntry;
        if (aligned && list + 1 == lists) {
            length = ((at + length + 3) & ~std::size_t{3}) - at;
        }
        at += length;
    }
    return at;
}

bool Rvz::ReadGroup(std::uint32_t index, const Region& region,
                    std::vector<std::uint8_t>& out) const {
    if (index >= mGroups.size()) {
        return false;
    }
    const Group& group = mGroups[index];

    // A group with no bytes is a run of zeros: a part of the disc that was
    // never written, or was scrubbed away.
    if (group.size == 0) {
        out.assign(region.chunk, 0);
        return true;
    }

    std::vector<std::uint8_t> raw(group.size);
    if (!mFile.Read(group.offset, raw.data(), raw.size())) {
        return false;
    }

    // Hash exceptions travel with the data they belong to: inside the
    // compressed stream when there is one, and in front of it when there is
    // not.
    const bool lists_compressed = group.compressed && mCompression != Compression::Purge;
    if (region.lists != 0 && !lists_compressed) {
        const std::size_t skip = SkipExceptionLists(raw.data(), raw.size(), region.lists, true);
        raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(std::min(skip, raw.size())));
    }

    if (!group.compressed) {
        out = std::move(raw);
    } else {
        if (mDecompressor.inflate == nullptr) {
            return false;
        }
        out.assign(region.chunk + region.lists * kListSlack, 0);
        const std::size_t written = mDecompressor.inflate(mDecompressor.context, mCompression,
                                                          raw.data(), raw.size(), out.data(),
                                                          out.size());
        if (written == 0) {
            return false;
        }
        out.resize(written);
    }

    if (region.lists != 0 && lists_compressed) {
        const std::size_t skip = SkipExceptionLists(out.data(), out.size(), region.lists, false);
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(std::min(skip, out.size())));
    }

    // An RVZ group may be "packed": the bytes that come out of the
    // decompressor are runs of real data and runs of the filler a disc carries
    // between its files, the latter recorded as a seed rather than as bytes.
    // `packed_size` says how much of the stream is that representation; what it
    // expands to is the chunk. The filler lies where no file does, so it is
    // written as zeros (see the header).
    if (group.packed_size != 0) {
        const std::size_t limit = std::min<std::size_t>(out.size(), group.packed_size);
        std::vector<std::uint8_t> unpacked;
        unpacked.reserve(region.chunk);

        std::size_t at = 0;
        while (at + 4 <= limit) {
            const std::uint32_t field = Read32(out.data() + at);
            at += 4;
            const std::size_t length = field & 0x7FFFFFFFu;
            if ((field & 0x80000000u) != 0) {
                at += 68;  // the seed the filler would be regrown from
                unpacked.insert(unpacked.end(), length, 0);
            } else {
                const std::size_t take = std::min(length, limit - at);
                unpacked.insert(unpacked.end(), out.begin() + static_cast<std::ptrdiff_t>(at),
                                out.begin() + static_cast<std::ptrdiff_t>(at + take));
                at += length;
            }
        }
        out = std::move(unpacked);
    }
    return true;
}

bool Rvz::ReadTables(const std::vector<std::uint8_t>& disc) {
    mConsole = Read32(disc.data()) == 1 ? Console::GameCube : Console::Wii;
    mCompression = static_cast<Compression>(Read32(disc.data() + 0x04));
    mChunkSize = Read32(disc.data() + 0x0C);
    std::memcpy(mDiscHeader, disc.data() + 0x10, sizeof(mDiscHeader));

    if (mChunkSize == 0 || (mChunkSize & (mChunkSize - 1)) != 0 || mChunkSize < kClusterSize) {
        return false;
    }

    const std::uint32_t partitions = Read32(disc.data() + 0x90);
    const std::uint32_t partition_entry = Read32(disc.data() + 0x94);
    const std::uint64_t partition_offset = Read64(disc.data() + 0x98);
    const std::uint32_t raw_entries = Read32(disc.data() + 0xB4);
    const std::uint64_t raw_offset = Read64(disc.data() + 0xB8);
    const std::uint32_t raw_size = Read32(disc.data() + 0xC0);
    const std::uint32_t groups = Read32(disc.data() + 0xC4);
    const std::uint64_t group_offset = Read64(disc.data() + 0xC8);
    const std::uint32_t group_size = Read32(disc.data() + 0xD0);
    const std::uint8_t parameters_size = disc[0xD4];

    mDecompressor.parameters = disc.data() + 0xD5;
    mDecompressor.parameters_size = std::min<std::size_t>(parameters_size, 7);

    // Everything but the partition table is compressed with the file's own
    // method, so those two tables cannot be read without a decompressor.
    const auto plain = [&](std::uint64_t offset, std::uint32_t stored, std::size_t expanded,
                           std::vector<std::uint8_t>& out) {
        std::vector<std::uint8_t> raw(stored);
        if (stored == 0 || !mFile.Read(offset, raw.data(), raw.size())) {
            return false;
        }
        if (mCompression == Compression::None) {
            out = std::move(raw);
            return out.size() >= expanded;
        }
        if (mDecompressor.inflate == nullptr) {
            return false;
        }
        out.assign(expanded, 0);
        return mDecompressor.inflate(mDecompressor.context, mCompression, raw.data(), raw.size(),
                                     out.data(), out.size()) >= expanded;
    };

    // The group table: where each chunk is, and how it is stored. RVZ carries a
    // third word per group and puts the "this one is compressed" flag in the
    // top bit of the size; WIA compresses every group.
    const bool rvz = mIsRvz;
    const std::size_t entry = rvz ? 12 : 8;
    std::vector<std::uint8_t> table;
    if (!plain(group_offset, group_size, groups * entry, table)) {
        return false;
    }
    mGroups.reserve(groups);
    for (std::uint32_t index = 0; index < groups; index++) {
        const std::uint8_t* at = table.data() + index * entry;
        Group group;
        group.offset = static_cast<std::uint64_t>(Read32(at)) << 2;
        const std::uint32_t size = Read32(at + 4);
        if (rvz) {
            group.compressed = (size & 0x80000000u) != 0;
            group.size = size & 0x7FFFFFFFu;
            group.packed_size = Read32(at + 8);
        } else {
            group.compressed = true;
            group.size = size;
        }
        mGroups.push_back(group);
    }

    // The raw-data regions: the disc outside its partitions.
    std::vector<std::uint8_t> raw_table;
    if (raw_entries != 0 && !plain(raw_offset, raw_size, raw_entries * kRawDataEntrySize,
                                   raw_table)) {
        return false;
    }
    for (std::uint32_t index = 0; index < raw_entries; index++) {
        const std::uint8_t* at = raw_table.data() + index * kRawDataEntrySize;
        Region region;
        region.offset = Read64(at);
        region.size = Read64(at + 8);
        region.first_group = Read32(at + 16);
        region.groups = Read32(at + 20);
        region.chunk = mChunkSize;

        // A region's chunks are laid on a grid of whole disc sectors, so a
        // region that starts inside one begins at the sector boundary below it
        // and is that much longer.
        const std::uint64_t skipped = region.offset % kClusterSize;
        region.offset -= skipped;
        region.size += skipped;
        mRegions.push_back(region);
    }

    // The partitions: each has a management segment and a data segment, and
    // both are stored the same way.
    if (partitions != 0 && partition_entry >= kPartitionEntrySize) {
        std::vector<std::uint8_t> partition_table(partitions * partition_entry);
        if (!mFile.Read(partition_offset, partition_table.data(), partition_table.size())) {
            return false;
        }
        // A partition's data is stored decrypted, so its chunks hold 0x7C00
        // for every 0x8000 of disc, and each carries the hashes it replaced as
        // exception lists in front of it.
        const std::uint32_t partition_chunk =
            static_cast<std::uint32_t>(mChunkSize / kClusterSize * kClusterStored);
        const std::uint32_t lists =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(partition_chunk / kGroupData));

        for (std::uint32_t index = 0; index < partitions; index++) {
            const std::uint8_t* at = partition_table.data() + index * partition_entry;
            // Everything in a partition is measured from its first sector.
            const std::uint64_t first = Read32(at + 16);
            const std::uint64_t start = first * kClusterSize;

            for (int segment = 0; segment < 2; segment++) {
                const std::uint8_t* data = at + 16 + segment * 16;
                Region region;
                region.offset = start + (Read32(data) - first) * kClusterStored;
                region.size = static_cast<std::uint64_t>(Read32(data + 4)) * kClusterStored;
                region.first_group = Read32(data + 8);
                region.groups = Read32(data + 12);
                region.chunk = partition_chunk;
                region.lists = lists;
                if (region.size != 0) {
                    mRegions.push_back(region);
                }
            }
        }
    }

    mSupported = mCompression == Compression::None || mDecompressor.inflate != nullptr;
    return true;
}

std::optional<Rvz> Rvz::Open(Source file, Decompressor decompressor) {
    std::uint8_t header[kFileHeaderSize];
    if (!file.Read(0, header, sizeof(header))) {
        return std::nullopt;
    }

    const bool wia = std::memcmp(header, "WIA\x01", 4) == 0;
    const bool rvz = std::memcmp(header, "RVZ\x01", 4) == 0;
    if (!wia && !rvz) {
        return std::nullopt;
    }

    Rvz image;
    image.mFile = file;
    image.mDecompressor = decompressor;
    image.mIsRvz = rvz;
    image.mDiscSize = Read64(header + 0x24);

    const std::uint32_t described = Read32(header + 0x0C);
    if (described < kDiscSize) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> disc(described);
    if (!file.Read(kFileHeaderSize, disc.data(), disc.size())) {
        return std::nullopt;
    }
    if (!image.ReadTables(disc)) {
        return std::nullopt;
    }
    return image;
}

bool Rvz::Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
    while (size != 0) {
        const Region* found = nullptr;
        for (const Region& region : mRegions) {
            // A partition's own offsets are counted in disc bytes; what is
            // stored is 0x7C00 for every 0x8000 of them.
            if (offset >= region.offset && offset < region.offset + region.size) {
                found = &region;
                break;
            }
        }
        if (found == nullptr) {
            // Outside everything the file describes: a disc reads as zeros
            // there.
            std::memset(out, 0, size);
            return true;
        }

        const std::uint64_t within = offset - found->offset;
        const std::uint32_t chunk = found->chunk;
        const std::uint32_t group =
            found->first_group + static_cast<std::uint32_t>(within / chunk);
        const std::size_t at = static_cast<std::size_t>(within % chunk);
        if (group >= found->first_group + found->groups) {
            return false;
        }

        if (mGroupIndex != group) {
            if (!ReadGroup(group, *found, mGroupBytes)) {
                return false;
            }
            mGroupIndex = group;
        }

        // A read stops at the end of the chunk it is in, and at the end of the
        // region: the next of either is a different group, or different bytes
        // entirely.
        const std::uint64_t left_in_region = found->offset + found->size - offset;
        const std::size_t left_in_chunk = chunk - at;
        std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>({size, left_in_region, left_in_chunk}));
        if (take == 0) {
            return false;
        }

        // A chunk may hold less than it covers - the last one of a region, or
        // one whose tail was scrubbed away. What it does not hold reads as
        // zeros, which is what that part of a disc is.
        if (at >= mGroupBytes.size()) {
            std::memset(out, 0, take);
            out += take;
            offset += take;
            size -= take;
            continue;
        }
        take = std::min(take, mGroupBytes.size() - at);
        std::memcpy(out, mGroupBytes.data() + at, take);
        out += take;
        offset += take;
        size -= take;
    }
    return true;
}

namespace {

bool ReadRvz(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    return static_cast<const Rvz*>(context)->ReadThrough(offset, out, size);
}

}  // namespace

Source Rvz::AsSource() const { return Source{const_cast<Rvz*>(this), &ReadRvz}; }

bool Rvz::ReadThrough(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
    return Read(offset, out, size);
}

}  // namespace wiinx::disc
