#pragma once

// Tickets and TMDs: what a title is, and what it is allowed to be.
//
// Every title in a NAND comes with two signed blobs. The ticket carries the
// title key, wrapped with a key the console holds; the TMD (title metadata)
// lists the title's contents - one record per file, with its id, its size and
// its SHA-1. Together they are what a WAD installs and what IOS checks before
// it runs anything.
//
// This parses and rebuilds them. It verifies no signature and holds no key: it
// reports the signature's type and issuer and leaves trust to the caller, and
// decryption to whoever has a key (see `TitleKeyIv` and `ContentIv`, which are
// format, not secret).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::nand {

// The signature that opens a ticket, a TMD or a certificate. Its type decides
// how many bytes it takes before the data starts.
enum class SignatureType : std::uint32_t {
    Rsa4096Sha1 = 0x00010000,
    Rsa2048Sha1 = 0x00010001,
    EcdsaSha1 = 0x00010002,
};

// Signature plus its padding, i.e. where the signed structure begins.
std::optional<std::size_t> SignatureSize(SignatureType type);

// A ticket: one title's key, and the rights that came with it.
struct Ticket {
    static constexpr std::size_t kBodySize = 0x164;

    SignatureType signature_type = SignatureType::Rsa2048Sha1;
    std::string issuer;                        // "Root-CA00000001-XS00000003"
    std::array<std::uint8_t, 16> title_key{};  // still wrapped with the common key
    std::uint64_t ticket_id = 0;
    std::uint32_t console_id = 0;
    std::uint64_t title_id = 0;
    std::uint16_t title_version = 0;
    std::uint8_t common_key_index = 0;         // 0 common, 1 Korean, 2 vWii
    std::vector<std::uint8_t> raw;             // the whole blob, signature included

    static std::optional<Ticket> Parse(const std::uint8_t* data, std::size_t size);
};

// One file of a title, as the TMD lists it.
struct ContentRecord {
    std::uint32_t id = 0;       // names the file: 0000001b.app
    std::uint16_t index = 0;    // its place in the title, and its decryption IV
    std::uint16_t type = 0;     // 1 normal, 2 hashed (streamed), 0x4001 shared
    std::uint64_t size = 0;     // the decrypted size; on disk it is padded to 16
    std::array<std::uint8_t, 20> sha1{};

    bool Shared() const { return (type & 0x8000) != 0 || type == 0x4001; }
    // What the content takes in a WAD: AES works in whole blocks.
    std::uint64_t EncryptedSize() const { return (size + 15) & ~static_cast<std::uint64_t>(15); }
};

// A TMD: the title, and every file it is made of.
struct Tmd {
    SignatureType signature_type = SignatureType::Rsa2048Sha1;
    std::string issuer;                  // "Root-CA00000001-CP00000004"
    std::uint8_t version = 0;
    std::uint64_t system_version = 0;    // the IOS this title wants
    std::uint64_t title_id = 0;
    std::uint32_t title_type = 0;
    std::uint16_t group_id = 0;
    std::uint16_t region = 0;
    std::uint32_t access_rights = 0;
    std::uint16_t title_version = 0;
    std::uint16_t boot_index = 0;        // which content the title starts at
    std::vector<ContentRecord> contents;
    std::vector<std::uint8_t> raw;       // the whole blob, signature included

    static std::optional<Tmd> Parse(const std::uint8_t* data, std::size_t size);

    const ContentRecord* Content(std::uint16_t index) const;
    const ContentRecord* BootContent() const { return Content(boot_index); }
    std::uint64_t TotalSize() const;     // every content, decrypted
};

// The two initialisation vectors the format defines. Neither is a secret; the
// keys they are used with are the caller's to supply.
//
//   title key:  AES-128-CBC with a common key, IV = the title id, zero-padded
//   content:    AES-128-CBC with the title key, IV = the content index, padded
std::array<std::uint8_t, 16> TitleKeyIv(std::uint64_t title_id);
std::array<std::uint8_t, 16> ContentIv(std::uint16_t content_index);

}  // namespace wiinx::nand
