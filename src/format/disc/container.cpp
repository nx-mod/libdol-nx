// Turning a container back into the image inside it.
//
// Two are read here, and they are the two that need no decompressor:
//
//   CISO   a header of one-byte flags, one per block, saying which blocks are
//          in the file at all. Absent blocks are blocks of zeros.
//   WBFS   the layout USB loaders keep games in. The disc is cut into sectors
//          and a table says which sector of the file holds each one, so the
//          empty parts of a disc take no space.
//
// The rest are identified and refused with their name, which is more use to
// whoever is holding the file than a failure to open it.
#include "wiinx/format/disc/container.hpp"

#include <algorithm>
#include <cstring>

namespace wiinx::disc {
namespace {

std::uint32_t Read32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint32_t Read32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t Read16BE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

constexpr std::size_t kCisoHeaderSize = 0x8000;
constexpr std::size_t kCisoBlocks = kCisoHeaderSize - 8;

}  // namespace

ContainerKind Identify(const Source& file) {
    std::uint8_t magic[8];
    if (!file.Read(0, magic, sizeof(magic))) {
        return ContainerKind::Unknown;
    }
    if (std::memcmp(magic, "CISO", 4) == 0) {
        return ContainerKind::Ciso;
    }
    if (std::memcmp(magic, "WBFS", 4) == 0) {
        return ContainerKind::Wbfs;
    }
    if (Read32BE(magic) == 0xB10BC001u) {
        return ContainerKind::Gcz;
    }
    if (std::memcmp(magic, "WIA\x01", 4) == 0) {
        return ContainerKind::Wia;
    }
    if (std::memcmp(magic, "RVZ\x01", 4) == 0) {
        return ContainerKind::Rvz;
    }
    // NKit writes its own marker inside an otherwise ordinary image.
    std::uint8_t marker[4];
    if (file.Read(0x200, marker, sizeof(marker)) && std::memcmp(marker, "NKIT", 4) == 0) {
        return ContainerKind::Nkit;
    }
    return ContainerKind::Raw;
}

std::string_view Name(ContainerKind kind) {
    switch (kind) {
        case ContainerKind::Raw: return "a raw image";
        case ContainerKind::Ciso: return "CISO";
        case ContainerKind::Wbfs: return "WBFS";
        case ContainerKind::Gcz: return "GCZ";
        case ContainerKind::Wia: return "WIA";
        case ContainerKind::Rvz: return "RVZ";
        case ContainerKind::Nkit: return "NKit";
        case ContainerKind::Unknown: break;
    }
    return "an unrecognised file";
}

bool Readable(ContainerKind kind) {
    return kind == ContainerKind::Raw || kind == ContainerKind::Ciso ||
           kind == ContainerKind::Wbfs;
}

bool Container::OpenCiso() {
    std::uint8_t header[kCisoHeaderSize];
    if (!mFile.Read(0, header, sizeof(header))) {
        return false;
    }
    mBlockSize = Read32LE(header + 4);
    if (mBlockSize == 0 || mBlockSize > 64u * 1024u * 1024u) {
        return false;
    }

    // One flag per block, in order. Present blocks follow the header back to
    // back, so where each one sits is its position among the present ones.
    std::uint64_t at = kCisoHeaderSize;
    mBlocks.reserve(kCisoBlocks);
    for (std::size_t index = 0; index < kCisoBlocks; index++) {
        if (header[8 + index] != 0) {
            mBlocks.push_back(at);
            at += mBlockSize;
        } else {
            mBlocks.push_back(0);  // a block of zeros, stored nowhere
        }
    }
    mImageSize = static_cast<std::uint64_t>(mBlocks.size()) * mBlockSize;
    return true;
}

bool Container::OpenWbfs() {
    std::uint8_t header[12];
    if (!mFile.Read(0, header, sizeof(header))) {
        return false;
    }
    const std::uint32_t hd_sector_size = 1u << header[8];
    const std::uint32_t wbfs_sector_size = 1u << header[9];
    if (header[8] < 9 || header[9] < 12 || header[9] > 30) {
        return false;
    }

    // The first game's entry is one hd sector in: its disc header, then a table
    // saying which sector of the file holds each sector of the disc.
    constexpr std::uint64_t kWiiDiscSize = 0x118240000ull;  // a dual-layer disc
    const std::size_t count = static_cast<std::size_t>(kWiiDiscSize / wbfs_sector_size);
    std::vector<std::uint8_t> table(count * 2);
    if (!mFile.Read(hd_sector_size + 0x100, table.data(), table.size())) {
        return false;
    }

    mBlockSize = wbfs_sector_size;
    mBlocks.reserve(count);
    for (std::size_t index = 0; index < count; index++) {
        const std::uint16_t sector = Read16BE(table.data() + index * 2);
        mBlocks.push_back(sector == 0 ? 0
                                      : static_cast<std::uint64_t>(sector) * wbfs_sector_size);
    }

    // Trailing sectors a game never wrote are absent; the image is as long as
    // the disc it came from.
    mImageSize = kWiiDiscSize;
    return true;
}

std::optional<Container> Container::Open(Source file) {
    Container container;
    container.mFile = file;
    container.mKind = Identify(file);

    switch (container.mKind) {
        case ContainerKind::Raw:
            break;
        case ContainerKind::Ciso:
            if (!container.OpenCiso()) {
                return std::nullopt;
            }
            break;
        case ContainerKind::Wbfs:
            if (!container.OpenWbfs()) {
                return std::nullopt;
            }
            break;
        default:
            // Identified, and not readable yet: the caller says so by name.
            return std::nullopt;
    }
    return container;
}

Source Container::AsSource() const {
    if (mKind == ContainerKind::Raw) {
        return mFile;
    }

    // A block map: every read is cut at block boundaries, and a block that is
    // not in the file is a block of zeros.
    struct Mapped {
        static bool Read(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
            const auto* self = static_cast<const Container*>(context);
            while (size != 0) {
                const std::uint64_t block = offset / self->mBlockSize;
                const std::size_t within = static_cast<std::size_t>(offset % self->mBlockSize);
                const std::size_t take = std::min<std::size_t>(size, self->mBlockSize - within);
                if (block >= self->mBlocks.size()) {
                    return false;
                }
                const std::uint64_t at = self->mBlocks[static_cast<std::size_t>(block)];
                if (at == 0) {
                    std::memset(out, 0, take);
                } else if (!self->mFile.Read(at + within, out, take)) {
                    return false;
                }
                out += take;
                offset += take;
                size -= take;
            }
            return true;
        }
    };
    return Source{const_cast<Container*>(this), &Mapped::Read};
}

}  // namespace wiinx::disc
