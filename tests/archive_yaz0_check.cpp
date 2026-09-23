// Yaz0: streams built here, expanded by the library, plus the malformed ones a
// game must survive.
#include "wiinx/format/archive/yaz0.hpp"

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

// A Yaz0 stream: the header, then groups of eight codes.
class Stream {
  public:
    explicit Stream(std::uint32_t expanded) {
        mBytes.assign({'Y', 'a', 'z', '0'});
        for (int shift = 24; shift >= 0; shift -= 8) {
            mBytes.push_back(static_cast<std::uint8_t>(expanded >> shift));
        }
        mBytes.resize(wiinx::archive::kYaz0HeaderSize, 0);
    }

    void Literal(std::uint8_t value) {
        Code(true);
        mBytes.push_back(value);
    }

    // A back-reference: `distance` bytes back, `count` bytes long.
    void Back(std::uint32_t distance, std::uint32_t count) {
        Code(false);
        const std::uint32_t encoded = distance - 1;
        if (count <= 17) {
            const std::uint32_t reference = ((count - 2) << 12) | encoded;
            mBytes.push_back(static_cast<std::uint8_t>(reference >> 8));
            mBytes.push_back(static_cast<std::uint8_t>(reference));
        } else {
            mBytes.push_back(static_cast<std::uint8_t>(encoded >> 8));
            mBytes.push_back(static_cast<std::uint8_t>(encoded));
            mBytes.push_back(static_cast<std::uint8_t>(count - 18));
        }
    }

    const std::vector<std::uint8_t>& Bytes() const { return mBytes; }

  private:
    void Code(bool literal) {
        if (mMask == 0) {
            mGroup = mBytes.size();
            mBytes.push_back(0);
            mMask = 0x80;
        }
        if (literal) {
            mBytes[mGroup] |= static_cast<std::uint8_t>(mMask);
        }
        mMask >>= 1;
    }

    std::vector<std::uint8_t> mBytes;
    std::size_t mGroup = 0;
    std::uint32_t mMask = 0;
};

std::string Expand(const std::vector<std::uint8_t>& stream, std::size_t expanded) {
    std::vector<std::uint8_t> out(expanded + 8, 0xCC);
    const std::uint32_t written =
        wiinx::archive::Yaz0Decode(stream.data(), stream.size(), out.data(), expanded);
    if (written == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(out.data()), written);
}
}  // namespace

int main() {
    std::printf("wiinx::archive Yaz0 check\n\n");

    Check("a non-Yaz0 buffer has no size",
          wiinx::archive::Yaz0ExpandedSize(reinterpret_cast<const std::uint8_t*>("NOPE0000"), 8) == 0);

    {   // literals only
        Stream stream(5);
        for (char value : std::string("HELLO")) {
            stream.Literal(static_cast<std::uint8_t>(value));
        }
        Check("literals", Expand(stream.Bytes(), 5) == "HELLO");
    }

    {   // a short back-reference
        Stream stream(8);
        stream.Literal('A');
        stream.Literal('B');
        stream.Back(2, 6);
        Check("back-reference", Expand(stream.Bytes(), 8) == "ABABABAB");
    }

    {   // an overlapping run: distance one repeats the byte
        Stream stream(6);
        stream.Literal('Z');
        stream.Back(1, 5);
        Check("overlapping run", Expand(stream.Bytes(), 6) == "ZZZZZZ");
    }

    {   // a long run, which carries its length in a third byte
        Stream stream(21);
        stream.Literal('Q');
        stream.Back(1, 20);
        Check("long run", Expand(stream.Bytes(), 21) == std::string(21, 'Q'));
    }

    {   // reaching back before the output: refused, not copied from elsewhere
        Stream stream(4);
        stream.Literal('A');
        stream.Back(2, 3);
        Check("reference before the output is refused", Expand(stream.Bytes(), 4).empty());
    }

    {   // a run longer than the declared size: refused
        Stream stream(4);
        stream.Literal('A');
        stream.Back(1, 10);
        Check("overrunning the declared size is refused", Expand(stream.Bytes(), 4).empty());
    }

    {   // a stream that stops in the middle
        Stream stream(64);
        stream.Literal('A');
        Check("a truncated stream is refused", Expand(stream.Bytes(), 64).empty());
    }

    {   // a destination too small for what the header declares
        Stream stream(5);
        for (char value : std::string("HELLO")) {
            stream.Literal(static_cast<std::uint8_t>(value));
        }
        std::vector<std::uint8_t> small(4);
        Check("a destination that does not fit is refused",
              wiinx::archive::Yaz0Decode(stream.Bytes().data(), stream.Bytes().size(), small.data(),
                                         small.size()) == 0);
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
