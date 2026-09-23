#pragma once

// Disc images, GameCube and Wii.
//
// Three layers, and a caller may stop at any of them:
//
//   container   the file as it sits on a card - raw (.iso, .gcm), WBFS, CISO
//   partition   the Wii's partition table, and the encryption over its clusters
//   filesystem  the FST both consoles share, and the files in it
//
// A Wii image is a GameCube image plus the middle layer, so one reader serves
// both. Nothing here opens a file or allocates a disc's worth of memory: a
// caller supplies a `Source` that answers reads, and this asks for what it
// needs. That is what lets the same code read a dump on a PC, a dump on an SD
// card, and a buffer in a test.
//
// Decryption is the caller's as well. A Wii partition is AES-CBC under a title
// key that is itself wrapped with a common key; both are supplied through
// `Cipher`, so no key is in this library.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::disc {

// Where the bytes come from. `read` returns false when the range is not there.
struct Source {
    void* context = nullptr;
    bool (*read)(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) = nullptr;

    bool Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
        return read != nullptr && read(context, offset, out, size);
    }
};

// A source over a buffer already in memory, for a test or a small file.
Source FromMemory(const std::uint8_t* data, std::size_t size);

// AES-128-CBC, which a Wii image needs and a GameCube image does not.
struct Cipher {
    void* context = nullptr;
    // Decrypts `size` bytes (a multiple of 16) in CBC mode. `out` may be `in`.
    void (*cbc_decrypt)(void* context, const std::uint8_t key[16], const std::uint8_t iv[16],
                        const std::uint8_t* in, std::uint8_t* out, std::size_t size) = nullptr;
    // The key a partition's title key is wrapped with. Which one is the ticket's
    // to say; the caller holds them.
    const std::uint8_t* (*common_key)(void* context, std::uint8_t index) = nullptr;
};

enum class Console {
    GameCube,
    Wii,
};

// The first bytes of any disc: what game this is, and where the rest lives.
struct Header {
    Console console = Console::GameCube;
    std::string id;       // "GM4E01", "RMCP01"
    std::string name;     // as the console shows it
    std::uint8_t disc = 0;
    std::uint8_t version = 0;
    std::uint32_t executable_offset = 0;  // the DOL
    std::uint32_t fst_offset = 0;
    std::uint32_t fst_size = 0;
};

// One file or folder in the disc's own table.
struct Entry {
    std::string path;          // "files/course/beginner_course.szs"
    bool directory = false;
    std::uint64_t offset = 0;  // within whatever this table describes
    std::uint32_t size = 0;
};

// A Wii disc's partitions. A GameCube disc has none.
struct Partition {
    std::uint32_t group = 0;
    std::uint32_t type = 0;      // 0 game, 1 update, 2 channel installer
    std::uint64_t offset = 0;    // where the partition starts on the disc

    bool Game() const { return type == 0; }
};

// A disc, read through whatever source was given.
// How to read what the source hands out.
struct Options {
    // Needed only for a Wii disc whose clusters are encrypted.
    Cipher cipher{};
    // True when a partition's data arrives already decrypted and without its
    // hash blocks - what a dump in Dolphin's formats gives, and what an
    // extracted partition is.
    bool partitions_plain = false;
};

class Image {
  public:
    // Reads the header. Returns nothing when the bytes are not a disc: the
    // magic at 0x1C says GameCube, the one at 0x18 says Wii.
    static std::optional<Image> Open(Source source, Cipher cipher = {});
    static std::optional<Image> Open(Source source, Options options);

    const Header& GetHeader() const { return mHeader; }
    Console GetConsole() const { return mHeader.console; }

    // A Wii disc's partitions, in the order the table lists them. Empty on a
    // GameCube disc.
    const std::vector<Partition>& Partitions() const { return mPartitions; }

    // Reads the disc's files. On a Wii disc this reads the game partition,
    // decrypting as it goes, so a caller sees the same thing either way.
    bool ReadFileTable();
    const std::vector<Entry>& Files() const { return mFiles; }
    const Entry* Find(const std::string& path) const;

    // Reads `size` bytes at `offset` within the game's own address space - what
    // an entry's `offset` is measured in.
    bool ReadData(std::uint64_t offset, std::uint8_t* out, std::size_t size);

    // The whole of one file.
    std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path);

    // The executable the disc boots, as bytes.
    std::optional<std::vector<std::uint8_t>> ReadExecutable();

  private:
    bool OpenGamePartition();
    bool ReadCluster(std::uint64_t cluster, std::uint8_t* out);

    Source mSource;
    Cipher mCipher;
    bool mPlainPartitions = false;
    Header mHeader;
    std::vector<Partition> mPartitions;
    std::vector<Entry> mFiles;

    // Wii only: where the game partition's data begins, and the key its
    // clusters are encrypted with.
    bool mEncrypted = false;
    std::uint64_t mDataOffset = 0;
    std::uint8_t mTitleKey[16] = {};
    // The last cluster decrypted, kept because a game reading a file reads the
    // same cluster many times over.
    std::vector<std::uint8_t> mCluster;
    std::uint64_t mClusterIndex = UINT64_MAX;
};

// A Wii partition's cluster: hashes first, then the data they cover.
inline constexpr std::size_t kClusterSize = 0x8000;
inline constexpr std::size_t kClusterHashSize = 0x400;
inline constexpr std::size_t kClusterDataSize = kClusterSize - kClusterHashSize;  // 0x7C00

}  // namespace wiinx::disc
