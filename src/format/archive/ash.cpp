// Expanding an ASH file.
//
//   0x00  "ASH0"
//   0x04  the expanded size, in the low 24 bits
//   0x08  where the distance stream begins
//   0x0C  where the symbol stream begins
//
// Two streams, read at once: symbols and lengths from one, distances from the
// other. Each begins with a tree, written as a walk - a 1 bit opens a branch, a
// 0 bit closes one with a symbol behind it - and then the coded data follows in
// the same stream.
//
// A symbol below 0x100 is a byte. Anything above is a length, and a distance is
// then read from the other stream to say how far back to copy from.
//
// Algorithm from ASH0-tools by Garhoogin and NinjaCheetah, MIT. Written here
// against this library's rule that malformed input is an answer rather than a
// crash: every read is bounded, and anything that does not add up returns 0
// instead of reading or writing outside the buffers given.
#include "wiinx/format/archive/ash.hpp"

#include <cstring>
#include <vector>

namespace wiinx::archive {
namespace {

constexpr std::uint32_t kMagic = 0x41534830;  // "ASH0"

// A node while a tree is being read: which side of its parent it hangs from,
// and which parent that is.
constexpr std::uint32_t kRight = 0x80000000;
constexpr std::uint32_t kLeft = 0x40000000;
constexpr std::uint32_t kIndex = 0x3FFFFFFF;

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

// Bits, most significant first, out of big-endian words. Running out of file is
// not an error to be reported later: the reader says so and everything above it
// stops.
class BitReader {
  public:
    BitReader(const std::uint8_t* data, std::size_t size, std::size_t at)
        : mData(data), mSize(size), mAt(at) {
        Feed();
    }

    bool Failed() const { return mFailed; }

    int Bit() {
        if (mFailed) {
            return 0;
        }
        const int bit = static_cast<int>(mWord >> 31);
        if (mUsed == 31) {
            Feed();
        } else {
            mUsed++;
            mWord <<= 1;
        }
        return bit;
    }

    std::uint32_t Bits(int count) {
        std::uint32_t value = 0;
        for (int index = 0; index < count; index++) {
            value = (value << 1) | static_cast<std::uint32_t>(Bit());
        }
        return value;
    }

  private:
    void Feed() {
        if (mAt + 4 > mSize) {
            mFailed = true;
            mWord = 0;
            mUsed = 0;
            return;
        }
        mWord = Read32(mData + mAt);
        mUsed = 0;
        mAt += 4;
    }

    const std::uint8_t* mData;
    std::size_t mSize;
    std::size_t mAt;
    std::uint32_t mWord = 0;
    std::uint32_t mUsed = 0;
    bool mFailed = false;
};

// One tree, as a pair of arrays: where a 0 bit leads and where a 1 bit leads.
// A value below `1 << width` is a symbol; anything above is another node.
struct Tree {
    std::vector<std::uint32_t> left;
    std::vector<std::uint32_t> right;
    std::uint32_t root = 0;
    bool ok = false;
};

Tree ReadTree(BitReader& reader, int width) {
    const std::uint32_t leaves = 1u << width;
    Tree tree;
    tree.left.assign(2 * leaves - 1, 0);
    tree.right.assign(2 * leaves - 1, 0);

    // The walk is kept on a stack: a branch opens two places to fill, and a
    // symbol closes the nearest of them.
    std::vector<std::uint32_t> pending;
    pending.reserve(2 * leaves);

    std::uint32_t next = leaves;
    std::uint32_t symbol = 0;
    std::size_t open = 0;

    do {
        if (reader.Failed() || next >= 2 * leaves - 1) {
            return tree;
        }
        if (reader.Bit() != 0) {
            pending.push_back(next | kRight);
            pending.push_back(next | kLeft);
            open += 2;
            next++;
        } else {
            symbol = reader.Bits(width);
            do {
                if (pending.empty()) {
                    return tree;
                }
                const std::uint32_t node = pending.back();
                pending.pop_back();
                const std::uint32_t index = node & kIndex;
                open--;
                if ((node & kRight) != 0) {
                    tree.right[index] = symbol;
                    symbol = index;
                } else {
                    tree.left[index] = symbol;
                    break;
                }
            } while (open > 0);
        }
    } while (open > 0);

    tree.root = symbol;
    tree.ok = !reader.Failed();
    return tree;
}

}  // namespace

bool IsAsh(const std::uint8_t* data, std::size_t size) {
    return data != nullptr && size >= kAshHeaderSize && Read32(data) == kMagic;
}

std::uint32_t AshExpandedSize(const std::uint8_t* data, std::size_t size) {
    return IsAsh(data, size) ? (Read32(data + 4) & 0x00FFFFFF) : 0;
}

std::uint32_t AshDecompress(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                            std::size_t dst_size, int symbol_bits, int distance_bits) {
    const std::uint32_t expanded = AshExpandedSize(src, src_size);
    if (expanded == 0 || dst == nullptr || dst_size < expanded) {
        return 0;
    }
    if (symbol_bits < 1 || symbol_bits > 16 || distance_bits < 1 || distance_bits > 16) {
        return 0;
    }

    // The symbols follow the header; the distances begin where it says.
    const std::size_t distance_at = Read32(src + 8);
    if (distance_at + 4 > src_size) {
        return 0;
    }
    BitReader symbols(src, src_size, kAshHeaderSize);
    BitReader distances(src, src_size, distance_at);

    const Tree symbol_tree = ReadTree(symbols, symbol_bits);
    const Tree distance_tree = ReadTree(distances, distance_bits);
    if (!symbol_tree.ok || !distance_tree.ok) {
        return 0;
    }

    const std::uint32_t symbol_leaves = 1u << symbol_bits;
    const std::uint32_t distance_leaves = 1u << distance_bits;

    std::uint32_t written = 0;
    while (written < expanded) {
        std::uint32_t symbol = symbol_tree.root;
        while (symbol >= symbol_leaves) {
            if (symbol >= symbol_tree.left.size() || symbols.Failed()) {
                return 0;
            }
            symbol = symbols.Bit() != 0 ? symbol_tree.right[symbol] : symbol_tree.left[symbol];
        }

        if (symbol < 0x100) {
            dst[written++] = static_cast<std::uint8_t>(symbol);
            continue;
        }

        std::uint32_t distance = distance_tree.root;
        while (distance >= distance_leaves) {
            if (distance >= distance_tree.left.size() || distances.Failed()) {
                return 0;
            }
            distance = distances.Bit() != 0 ? distance_tree.right[distance]
                                            : distance_tree.left[distance];
        }

        const std::uint32_t length = (symbol - 0x100) + 3;
        if (distance + 1 > written || written + length > expanded) {
            // A copy from before the output, or past what the header declared.
            return 0;
        }
        std::uint32_t from = written - distance - 1;
        for (std::uint32_t index = 0; index < length; index++) {
            dst[written++] = dst[from++];
        }
    }

    return symbols.Failed() || distances.Failed() ? 0 : written;
}

}  // namespace wiinx::archive
