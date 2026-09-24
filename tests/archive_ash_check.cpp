// ASH: a file built here, expanded back, plus the shapes that must be refused.
//
// Building one means writing the two trees the way the format writes them - as
// a walk, a 1 bit opening a branch and a 0 bit closing one with its symbol -
// which is also the clearest way to show what the reader is doing.
#include "wiinx/format/archive/ash.hpp"

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

// Bits, most significant first, into big-endian words - which is how the
// reader takes them out again.
class BitWriter {
  public:
    void Bit(int bit) {
        mWord = (mWord << 1) | static_cast<std::uint32_t>(bit & 1);
        if (++mBits == 32) {
            Flush();
        }
    }

    void Bits(std::uint32_t value, int count) {
        for (int index = count - 1; index >= 0; index--) {
            Bit(static_cast<int>((value >> index) & 1));
        }
    }

    // Whole words only: the reader takes four bytes at a time.
    std::vector<std::uint8_t> Finish() {
        while (mBits != 0) {
            Bit(0);
        }
        return mBytes;
    }

  private:
    void Flush() {
        for (int shift = 24; shift >= 0; shift -= 8) {
            mBytes.push_back(static_cast<std::uint8_t>(mWord >> shift));
        }
        mWord = 0;
        mBits = 0;
    }

    std::vector<std::uint8_t> mBytes;
    std::uint32_t mWord = 0;
    int mBits = 0;
};

// The symbols this test's tree carries: 'A' to the left, then 'B' and one
// length symbol behind a second branch.
//
//   A       0
//   B       10
//   len 3   11
void WriteSymbolTree(BitWriter& out) {
    out.Bit(1);                        // a branch
    out.Bit(0);  out.Bits('A', 9);     // its left: a literal
    out.Bit(1);                        // its right: another branch
    out.Bit(0);  out.Bits('B', 9);     // whose left is a literal
    out.Bit(0);  out.Bits(0x100, 9);   // and whose right is a length of three
}

// Two distances, one bit apart.
void WriteDistanceTree(BitWriter& out) {
    out.Bit(1);
    out.Bit(0);  out.Bits(0, 11);
    out.Bit(0);  out.Bits(1, 11);
}

// "AB" and then three bytes copied from one back, which is "BBB".
std::vector<std::uint8_t> BuildAsh(std::uint32_t expanded) {
    BitWriter symbols;
    WriteSymbolTree(symbols);
    symbols.Bit(0);                    // A
    symbols.Bit(1); symbols.Bit(0);    // B
    symbols.Bit(1); symbols.Bit(1);    // a length of three
    const std::vector<std::uint8_t> symbol_bytes = symbols.Finish();

    BitWriter distances;
    WriteDistanceTree(distances);
    distances.Bit(0);                  // distance 0, which is one byte back
    const std::vector<std::uint8_t> distance_bytes = distances.Finish();

    std::vector<std::uint8_t> file;
    const char* magic = "ASH0";
    file.insert(file.end(), magic, magic + 4);
    for (int shift = 24; shift >= 0; shift -= 8) {
        file.push_back(static_cast<std::uint8_t>(expanded >> shift));
    }

    const std::uint32_t distance_at =
        static_cast<std::uint32_t>(0x0C + symbol_bytes.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        file.push_back(static_cast<std::uint8_t>(distance_at >> shift));
    }

    file.insert(file.end(), symbol_bytes.begin(), symbol_bytes.end());
    file.insert(file.end(), distance_bytes.begin(), distance_bytes.end());
    return file;
}

std::string Expand(const std::vector<std::uint8_t>& file, std::size_t room) {
    std::vector<std::uint8_t> out(room, 0xCC);
    const std::uint32_t written =
        wiinx::archive::AshDecompress(file.data(), file.size(), out.data(), out.size());
    return written == 0 ? std::string{}
                        : std::string(reinterpret_cast<const char*>(out.data()), written);
}
}  // namespace

int main() {
    using namespace wiinx::archive;
    std::printf("wiinx::archive ASH check\n\n");

    const auto file = BuildAsh(5);

    Check("an ASH file is recognised", IsAsh(file.data(), file.size()));
    Check("something else is not",
          !IsAsh(reinterpret_cast<const std::uint8_t*>("NOPE12345678"), 12));
    Check("its size is what the header says", AshExpandedSize(file.data(), file.size()) == 5);

    Check("literals and a copy", Expand(file, 5) == "ABBBB");

    {   // A size the file cannot fill: refused rather than half-written.
        const auto larger = BuildAsh(64);
        Check("a file that runs out before its declared size is refused",
              Expand(larger, 64).empty());
    }

    {   // Room for less than it declares.
        std::vector<std::uint8_t> out(4, 0);
        Check("a destination that does not fit is refused",
              AshDecompress(file.data(), file.size(), out.data(), out.size()) == 0);
    }

    {   // A file cut short.
        std::vector<std::uint8_t> cut(file.begin(), file.begin() + 0x10);
        Check("a file cut short is refused", Expand(cut, 5).empty());
    }

    {   // A header pointing its distance stream past the end.
        std::vector<std::uint8_t> lying = file;
        lying[8] = 0x7F;
        Check("a distance stream outside the file is refused", Expand(lying, 5).empty());
    }

    {   // Widths nothing uses.
        std::vector<std::uint8_t> out(8, 0);
        Check("an impossible symbol width is refused",
              AshDecompress(file.data(), file.size(), out.data(), out.size(), 0, 11) == 0);
        Check("and an impossible distance width",
              AshDecompress(file.data(), file.size(), out.data(), out.size(), 9, 99) == 0);
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
