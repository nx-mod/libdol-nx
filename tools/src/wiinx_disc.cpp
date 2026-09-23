// Read a disc image: what it is, what is in it, and what is needed out of it.
//
//     wiinx-disc info    <image>              what it is, and what it carries
//     wiinx-disc list    <image>              every file on the disc
//     wiinx-disc dol     <image> [out]        the executable, for the translator
//     wiinx-disc file    <image> <path> [out] one file off the disc
//     wiinx-disc extract <image> <folder>     the whole disc, as a game project
//                                             expects it: sys/ and files/
//
// Reads what format/disc reads: raw images, CISO, WBFS, RVZ and WIA - the last
// two through Zstandard, which is what they use in practice. Nothing else is
// needed: no Dolphin, and no second copy of the disc.
#include "wiinx/format/disc/container.hpp"
#include "wiinx/format/disc/image.hpp"
#include "wiinx/format/disc/rvz.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(WIINX_HAVE_ZSTD)
#include <zstd.h>
#endif

using namespace wiinx::disc;

namespace {

// A source over a file on this machine.
bool ReadFile(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    auto* file = static_cast<std::FILE*>(context);
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out, 1, size, file) == size;
}

std::size_t Inflate(void*, Compression method, const std::uint8_t* in,
                    std::size_t in_size, std::uint8_t* out, std::size_t out_size) {
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

const char* ConsoleName(Console console) { return console == Console::Wii ? "Wii" : "GameCube"; }

}  // namespace

namespace {

// Everything a disc image is, once it has been opened: the container it was
// stored in, and the disc inside.
struct Opened {
    std::FILE* file = nullptr;
    std::optional<Rvz> rvz;
    std::optional<Container> container;
    std::optional<Image> image;
};

bool Open(const char* path, Opened& opened, bool quiet) {
    opened.file = std::fopen(path, "rb");
    if (opened.file == nullptr) {
        std::printf("cannot open %s\n", path);
        return false;
    }
    const Source source{opened.file, &ReadFile};
    const ContainerKind kind = Identify(source);
    if (!quiet) {
        std::printf(">> %s\n   container: %.*s\n", path, static_cast<int>(Name(kind).size()),
                    Name(kind).data());
    }

    if (kind == ContainerKind::Rvz || kind == ContainerKind::Wia) {
        Decompressor decompressor;
        decompressor.inflate = &Inflate;
        opened.rvz = Rvz::Open(source, decompressor);
        if (!opened.rvz) {
            std::printf("   could not read it\n");
            return false;
        }
        if (!opened.rvz->Supported()) {
            std::printf("   this build has no decompressor for its chunks\n");
            return false;
        }
        if (!quiet) {
            std::printf("   disc:      %s, %" PRIu64 " bytes\n",
                        ConsoleName(opened.rvz->GetConsole()), opened.rvz->DiscSize());
        }
        opened.image = Image::Open(opened.rvz->AsSource(), Options{{}, opened.rvz->Decrypted()});
    } else {
        opened.container = Container::Open(source);
        if (!opened.container) {
            std::printf("   %.*s is not a container this build reads yet\n",
                        static_cast<int>(Name(kind).size()), Name(kind).data());
            return false;
        }
        opened.image = Image::Open(opened.container->AsSource());
    }

    if (!opened.image) {
        std::printf("   no disc inside\n");
        return false;
    }
    return true;
}

void PrintHeader(const Image& image) {
    const Header& header = image.GetHeader();
    std::printf("   id:        %s\n   name:      %s\n   console:   %s\n", header.id.c_str(),
                header.name.c_str(), ConsoleName(header.console));
    std::printf("   dol at:    0x%08x    fst at: 0x%08x  (%u bytes)\n", header.executable_offset,
                header.fst_offset, header.fst_size);
    if (!image.Partitions().empty()) {
        std::printf("   partitions:");
        for (const Partition& partition : image.Partitions()) {
            std::printf(" type %u at 0x%" PRIx64, partition.type, partition.offset);
        }
        std::printf("\n");
    }
}

bool Write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::FILE* handle = std::fopen(path.string().c_str(), "wb");
    if (handle == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), handle) == bytes.size();
    std::fclose(handle);
    return ok;
}

// The whole disc, laid out the way a game project reads it: the executable and
// the disc's own headers in sys/, everything else under files/.
int Extract(Image& image, const std::filesystem::path& folder) {
    if (!image.ReadFileTable()) {
        std::printf("   could not read the file table\n");
        return 1;
    }

    const auto executable = image.ReadExecutable();
    if (!executable || !Write(folder / "sys" / "main.dol", *executable)) {
        std::printf("   could not write the executable\n");
        return 1;
    }
    std::printf("   sys/main.dol  %zu bytes\n", executable->size());

    // boot.bin and bi2.bin are the disc's own first bytes, which the SDK reads
    // back while a game runs.
    std::vector<std::uint8_t> boot(0x440);
    std::vector<std::uint8_t> bi2(0x2000);
    if (image.ReadData(0, boot.data(), boot.size())) {
        Write(folder / "sys" / "boot.bin", boot);
    }
    if (image.ReadData(0x440, bi2.data(), bi2.size())) {
        Write(folder / "sys" / "bi2.bin", bi2);
    }

    std::size_t written = 0;
    std::uint64_t bytes = 0;
    for (const Entry& entry : image.Files()) {
        if (entry.directory) {
            continue;
        }
        const auto contents = image.ReadFile(entry.path);
        if (!contents) {
            std::printf("   %s: could not be read\n", entry.path.c_str());
            return 1;
        }
        if (!Write(folder / "files" / entry.path, *contents)) {
            std::printf("   %s: could not be written\n", entry.path.c_str());
            return 1;
        }
        written++;
        bytes += contents->size();
        if (written % 100 == 0) {
            std::printf("   %zu files, %.0f MB\r", written, bytes / (1024.0 * 1024.0));
            std::fflush(stdout);
        }
    }
    std::printf("   files/        %zu files, %.1f MB\n", written, bytes / (1024.0 * 1024.0));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf(
            "usage:\n"
            "  wiinx-disc info    <image>\n"
            "  wiinx-disc list    <image>\n"
            "  wiinx-disc dol     <image> [out]\n"
            "  wiinx-disc file    <image> <path> [out]\n"
            "  wiinx-disc extract <image> <folder>\n");
        return 2;
    }

    const std::string command = argv[1];
    Opened opened;
    if (!Open(argv[2], opened, command == "dol" || command == "file")) {
        return 1;
    }
    Image& image = *opened.image;

    if (command == "info") {
        PrintHeader(image);
        if (!image.ReadFileTable()) {
            std::printf("   could not read the file table\n");
            return 1;
        }
        std::uint64_t total = 0;
        std::size_t folders = 0;
        for (const Entry& entry : image.Files()) {
            total += entry.size;
            folders += entry.directory ? 1 : 0;
        }
        std::printf("   files:     %zu in %zu folders, %.1f MB\n", image.Files().size() - folders,
                    folders, total / (1024.0 * 1024.0));
        const auto executable = image.ReadExecutable();
        std::printf("   executable: %s\n",
                    executable ? (std::to_string(executable->size()) + " bytes").c_str()
                               : "could not be read");
        return 0;
    }

    if (command == "list") {
        if (!image.ReadFileTable()) {
            std::printf("   could not read the file table\n");
            return 1;
        }
        for (const Entry& entry : image.Files()) {
            if (!entry.directory) {
                std::printf("%10u  %s\n", entry.size, entry.path.c_str());
            }
        }
        return 0;
    }

    if (command == "dol") {
        const auto executable = image.ReadExecutable();
        if (!executable) {
            std::printf("could not read the executable\n");
            return 1;
        }
        const char* out = argc > 3 ? argv[3] : "main.dol";
        if (!Write(out, *executable)) {
            return 1;
        }
        std::printf("%s: %zu bytes\n", out, executable->size());
        return 0;
    }

    if (command == "file") {
        if (argc < 4) {
            std::printf("which file?\n");
            return 2;
        }
        if (!image.ReadFileTable()) {
            return 1;
        }
        const auto bytes = image.ReadFile(argv[3]);
        if (!bytes) {
            std::printf("%s: not on this disc\n", argv[3]);
            return 1;
        }
        // Where it goes: what was asked for, or the file's own name here.
        std::string name;
        if (argc > 4) {
            name = argv[4];
        } else {
            const char* slash = std::strrchr(argv[3], '/');
            name = slash != nullptr ? slash + 1 : argv[3];
        }
        if (!Write(name, *bytes)) {
            return 1;
        }
        std::printf("%s: %zu bytes\n", name.c_str(), bytes->size());
        return 0;
    }

    if (command == "extract") {
        if (argc < 4) {
            std::printf("extract where?\n");
            return 2;
        }
        PrintHeader(image);
        return Extract(image, argv[3]);
    }

    std::printf("no such command: %s\n", command.c_str());
    return 2;
}
