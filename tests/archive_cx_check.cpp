// CX compression: streams built here, expanded back, plus the malformed ones.
#include "wiinx/format/archive/cx.hpp"

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

// A stream in either sliding-window kind: the header, then groups of eight
// items with a flag byte in front of each.
class Stream {
  public:
    Stream(wiinx::archive::CxKind kind, std::uint32_t expanded) : mKind(kind) {
        mBytes.push_back(static_cast<std::uint8_t>(kind));
        mBytes.push_back(static_cast<std::uint8_t>(expanded));
        mBytes.push_back(static_cast<std::uint8_t>(expanded >> 8));
        mBytes.push_back(static_cast<std::uint8_t>(expanded >> 16));
    }

    void Literal(std::uint8_t value) {
        Flag(false);
        mBytes.push_back(value);
    }

    // A run: `length` bytes from `distance` back.
    void Back(std::uint32_t distance, std::uint32_t length) {
        Flag(true);
        const std::uint32_t encoded = distance - 1;
        if (mKind == wiinx::archive::CxKind::Lz10) {
            mBytes.push_back(static_cast<std::uint8_t>(((length - 3) << 4) | (encoded >> 8)));
            mBytes.push_back(static_cast<std::uint8_t>(encoded));
        } else if (length <= 0x10) {
            // Two bytes: the length in the top nibble.
            mBytes.push_back(static_cast<std::uint8_t>(((length - 1) << 4) | (encoded >> 8)));
            mBytes.push_back(static_cast<std::uint8_t>(encoded));
        } else if (length <= 0x110) {
            // Three: eight bits of length, and the top nibble left at zero to
            // say which form this is.
            const std::uint32_t value = length - 0x11;
            mBytes.push_back(static_cast<std::uint8_t>(value >> 4));
            mBytes.push_back(static_cast<std::uint8_t>((value << 4) | (encoded >> 8)));
            mBytes.push_back(static_cast<std::uint8_t>(encoded));
        } else {
            // Four: sixteen bits of length, with 1 in the top nibble.
            const std::uint32_t value = length - 0x111;
            mBytes.push_back(static_cast<std::uint8_t>(0x10 | (value >> 12)));
            mBytes.push_back(static_cast<std::uint8_t>(value >> 4));
            mBytes.push_back(static_cast<std::uint8_t>((value << 4) | (encoded >> 8)));
            mBytes.push_back(static_cast<std::uint8_t>(encoded));
        }
    }

    const std::vector<std::uint8_t>& Bytes() const { return mBytes; }

  private:
    void Flag(bool reference) {
        if (mLeft == 0) {
            mGroup = mBytes.size();
            mBytes.push_back(0);
            mLeft = 8;
        }
        if (reference) {
            mBytes[mGroup] |= static_cast<std::uint8_t>(0x80 >> (8 - mLeft));
        }
        mLeft--;
    }

    wiinx::archive::CxKind mKind;
    std::vector<std::uint8_t> mBytes;
    std::size_t mGroup = 0;
    int mLeft = 0;
};

std::string Expand(const std::vector<std::uint8_t>& stream, std::size_t expanded) {
    std::vector<std::uint8_t> out(expanded, 0xCC);
    const std::uint32_t written =
        wiinx::archive::CxDecompress(stream.data(), stream.size(), out.data(), out.size());
    return written == 0 ? std::string{}
                        : std::string(reinterpret_cast<const char*>(out.data()), written);
}
}  // namespace

int main() {
    using namespace wiinx::archive;
    std::printf("wiinx::archive CX check\n\n");

    Check("a buffer that is not compressed says so",
          CxIdentify(reinterpret_cast<const std::uint8_t*>("NOPE"), 4) == CxKind::None);

    for (const CxKind kind : {CxKind::Lz10, CxKind::Lz11}) {
        const char* name = kind == CxKind::Lz10 ? "LZ77" : "LZ11";
        std::printf("  -- %s\n", name);

        {   // literals only
            Stream stream(kind, 5);
            for (char value : std::string("HELLO")) {
                stream.Literal(static_cast<std::uint8_t>(value));
            }
            Check("literals", Expand(stream.Bytes(), 5) == "HELLO");
        }

        {   // a run back over what was written
            Stream stream(kind, 8);
            stream.Literal('A');
            stream.Literal('B');
            stream.Back(2, 6);
            Check("back-reference", Expand(stream.Bytes(), 8) == "ABABABAB");
        }

        {   // an overlapping run: a distance of one repeats the byte
            Stream stream(kind, 6);
            stream.Literal('Z');
            stream.Back(1, 5);
            Check("overlapping run", Expand(stream.Bytes(), 6) == "ZZZZZZ");
        }

        {   // more than eight items, so the flags roll over
            Stream stream(kind, 12);
            for (char value : std::string("ABCDEFGHIJKL")) {
                stream.Literal(static_cast<std::uint8_t>(value));
            }
            Check("a second group of flags", Expand(stream.Bytes(), 12) == "ABCDEFGHIJKL");
        }

        {   // reaching back before the output
            Stream stream(kind, 4);
            stream.Literal('A');
            stream.Back(2, 3);
            Check("a reference before the output is refused", Expand(stream.Bytes(), 4).empty());
        }

        {   // a stream that stops in the middle
            Stream stream(kind, 64);
            stream.Literal('A');
            Check("a truncated stream is refused", Expand(stream.Bytes(), 64).empty());
        }
    }

    {   // LZ11's three-byte form: a run longer than a nibble can say
        Stream stream(CxKind::Lz11, 0x80);
        stream.Literal('X');
        stream.Back(1, 0x7F);
        Check("LZ11 medium run", Expand(stream.Bytes(), 0x80) == std::string(0x80, 'X'));
    }

    {   // LZ11's four-byte form: longer than the three-byte one can say
        Stream stream(CxKind::Lz11, 0x400);
        stream.Literal('Y');
        stream.Back(1, 0x3FF);
        Check("LZ11 long run", Expand(stream.Bytes(), 0x400) == std::string(0x400, 'Y'));
    }

    {   // a destination too small for what the header declares
        Stream stream(CxKind::Lz11, 5);
        for (char value : std::string("HELLO")) {
            stream.Literal(static_cast<std::uint8_t>(value));
        }
        std::vector<std::uint8_t> small(4);
        Check("a destination that does not fit is refused",
              CxDecompress(stream.Bytes().data(), stream.Bytes().size(), small.data(),
                           small.size()) == 0);
    }

    {   // a kind this does not expand
        const std::uint8_t huffman[8] = {0x28, 0x10, 0x00, 0x00, 0, 0, 0, 0};
        Check("Huffman is recognised", CxIdentify(huffman, sizeof(huffman)) == CxKind::Huffman8);
        std::vector<std::uint8_t> out(0x10);
        Check("  and declined rather than guessed at",
              CxDecompress(huffman, sizeof(huffman), out.data(), out.size()) == 0);
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
