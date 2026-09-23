// Look inside a disc image.
//
//     wiinx-inspect-image <file> [path/to/extract]
//     wiinx-inspect-image <file> --dol [output]
//
// Says what the file is, what disc is inside it, and what that disc carries.
// Reads what format/disc reads: raw images, CISO, WBFS, RVZ and WIA - the last
// two through Zstandard, which is what they use in practice.
#include "wiinx/format/disc/container.hpp"
#include "wiinx/format/disc/image.hpp"
#include "wiinx/format/disc/rvz.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(WIINX_HAVE_ZSTD)
#include <zstd.h>
#endif

namespace {

// A source over a file on this machine.
bool ReadFile(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    auto* file = static_cast<std::FILE*>(context);
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out, 1, size, file) == size;
}

std::size_t Inflate(void*, wiinx::disc::Compression method, const std::uint8_t* in,
                    std::size_t in_size, std::uint8_t* out, std::size_t out_size) {
    using wiinx::disc::Compression;
    if (method == Compression::None) {
        const std::size_t take = in_size < out_size ? in_size : out_size;
        std::memcpy(out, in, take);
        return take;
    }
#if defined(WIINX_HAVE_ZSTD)
    if (method == Compression::Zstd) {
        const std::size_t written = ZSTD_decompress(out, out_size, in, in_size);
        return ZSTD_isError(written) ? 0 : written;
    }
#endif
    return 0;  // a method this build does not carry
}

const char* ConsoleName(wiinx::disc::Console console) {
    return console == wiinx::disc::Console::Wii ? "Wii" : "GameCube";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace wiinx::disc;

    if (argc < 2) {
        std::printf("usage: wiinx-inspect-image <file> [path/to/extract]\n");
        return 2;
    }

    std::FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) {
        std::printf("cannot open %s\n", argv[1]);
        return 1;
    }
    const Source source{file, &ReadFile};

    const ContainerKind kind = Identify(source);
    std::printf(">> %s\n   container: %.*s\n", argv[1], static_cast<int>(Name(kind).size()),
                Name(kind).data());

    // RVZ and WIA carry the disc in compressed chunks and hand it back
    // decrypted; everything else is a block map over the image itself.
    std::optional<Rvz> rvz;
    std::optional<Container> container;
    std::optional<Image> image;

    if (kind == ContainerKind::Rvz || kind == ContainerKind::Wia) {
        Decompressor decompressor;
        decompressor.inflate = &Inflate;
        rvz = Rvz::Open(source, decompressor);
        if (!rvz) {
            std::printf("   could not read it\n");
            return 1;
        }
        std::printf("   disc:      %s, %" PRIu64 " bytes\n", ConsoleName(rvz->GetConsole()),
                    rvz->DiscSize());
        if (!rvz->Supported()) {
            std::printf("   this build has no decompressor for its chunks\n");
            return 1;
        }
        image = Image::Open(rvz->AsSource(), Options{{}, rvz->Decrypted()});
    } else {
        container = Container::Open(source);
        if (!container) {
            std::printf("   this build cannot read that container yet\n");
            return 1;
        }
        image = Image::Open(container->AsSource());
    }

    if (!image) {
        // Say what did come back, which is usually enough to see where the
        // reading went wrong.
        const Source inside = rvz ? rvz->AsSource() : container->AsSource();
        std::uint8_t head[32] = {};
        const bool read = inside.Read(0, head, sizeof(head));
        std::printf("   no disc inside (first bytes %s)\n", read ? "" : "could not be read");
        if (read) {
            for (unsigned char byte : head) {
                std::printf("%02x", byte);
            }
            std::printf("\n");
        }
        return 1;
    }

    const Header& header = image->GetHeader();
    std::printf("   id:        %s\n   name:      %s\n   console:   %s\n", header.id.c_str(),
                header.name.c_str(), ConsoleName(header.console));
    std::printf("   dol at:    0x%08x    fst at: 0x%08x  (%u bytes)\n", header.executable_offset,
                header.fst_offset, header.fst_size);
    if (!image->Partitions().empty()) {
        std::printf("   partitions:");
        for (const Partition& partition : image->Partitions()) {
            std::printf(" type %u at 0x%" PRIx64, partition.type, partition.offset);
        }
        std::printf("\n");
    }

    if (!image->ReadFileTable()) {
        std::printf("   could not read the file table\n");
        return 1;
    }

    std::uint64_t total = 0;
    std::size_t folders = 0;
    for (const Entry& entry : image->Files()) {
        total += entry.size;
        folders += entry.directory ? 1 : 0;
    }
    std::printf("   files:     %zu in %zu folders, %.1f MB\n", image->Files().size() - folders,
                folders, static_cast<double>(total) / (1024.0 * 1024.0));

    std::size_t shown = 0;
    for (const Entry& entry : image->Files()) {
        if (entry.directory || shown++ >= 8) {
            continue;
        }
        std::printf("     %-40s %10u\n", entry.path.c_str(), entry.size);
    }

    const auto executable = image->ReadExecutable();
    std::printf("   executable: %s\n",
                executable ? (std::to_string(executable->size()) + " bytes").c_str()
                           : "could not read");

    if (argc > 2 && std::strcmp(argv[2], "--dol") == 0) {
        // The disc's executable, which is where a game project starts.
        if (!executable) {
            std::printf("   could not read the executable\n");
            return 1;
        }
        const char* out = argc > 3 ? argv[3] : "main.dol";
        std::FILE* handle = std::fopen(out, "wb");
        if (handle == nullptr) {
            return 1;
        }
        std::fwrite(executable->data(), 1, executable->size(), handle);
        std::fclose(handle);
        std::printf("   wrote %s, %zu bytes\n", out, executable->size());
    } else if (argc > 2) {
        const auto bytes = image->ReadFile(argv[2]);
        if (!bytes) {
            std::printf("   %s: not on this disc\n", argv[2]);
            return 1;
        }
        const std::string out = std::string(std::strrchr(argv[2], '/') ? std::strrchr(argv[2], '/') + 1
                                                                       : argv[2]);
        std::FILE* handle = std::fopen(out.c_str(), "wb");
        if (handle == nullptr) {
            return 1;
        }
        std::fwrite(bytes->data(), 1, bytes->size(), handle);
        std::fclose(handle);
        std::printf("   wrote %s, %zu bytes\n", out.c_str(), bytes->size());
    }

    std::fclose(file);
    return 0;
}
