#include "wiinx/format/nand/title.hpp"

#include <cstring>

namespace wiinx::nand {
namespace {

std::uint16_t Read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint64_t Read64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(Read32(p)) << 32) | Read32(p + 4);
}

// The issuer is a fixed-width field padded with zeros.
std::string ReadIssuer(const std::uint8_t* p) {
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(p, 0, 0x40));
    return std::string(reinterpret_cast<const char*>(p),
                       end != nullptr ? static_cast<std::size_t>(end - p) : 0x40);
}

// Offsets inside a ticket, from the end of its signature.
constexpr std::size_t kTicketIssuer = 0x00;
constexpr std::size_t kTicketTitleKey = 0x7F;
constexpr std::size_t kTicketId = 0x90;
constexpr std::size_t kTicketConsoleId = 0x98;
constexpr std::size_t kTicketTitleId = 0x9C;
constexpr std::size_t kTicketTitleVersion = 0xA6;
constexpr std::size_t kTicketCommonKeyIndex = 0xB1;

// Offsets inside a TMD, from the end of its signature.
constexpr std::size_t kTmdIssuer = 0x00;
constexpr std::size_t kTmdVersion = 0x40;
constexpr std::size_t kTmdSystemVersion = 0x44;
constexpr std::size_t kTmdTitleId = 0x4C;
constexpr std::size_t kTmdTitleType = 0x54;
constexpr std::size_t kTmdGroupId = 0x58;
constexpr std::size_t kTmdRegion = 0x5C;
constexpr std::size_t kTmdAccessRights = 0x98;
constexpr std::size_t kTmdTitleVersion = 0x9C;
constexpr std::size_t kTmdContentCount = 0x9E;
constexpr std::size_t kTmdBootIndex = 0xA0;
constexpr std::size_t kTmdContents = 0xA4;
constexpr std::size_t kContentRecordSize = 0x24;

}  // namespace

std::optional<std::size_t> SignatureSize(SignatureType type) {
    switch (type) {
        // Every one is the signature itself plus padding out to a 64-byte
        // boundary, so the structure that follows starts aligned.
        case SignatureType::Rsa4096Sha1:
            return 0x200 + 0x3C;
        case SignatureType::Rsa2048Sha1:
            return 0x100 + 0x3C;
        case SignatureType::EcdsaSha1:
            return 0x3C + 0x40;
    }
    return std::nullopt;
}

std::optional<Ticket> Ticket::Parse(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return std::nullopt;
    }
    const auto type = static_cast<SignatureType>(Read32(data));
    const auto signature = SignatureSize(type);
    if (!signature || size < *signature + kBodySize) {
        return std::nullopt;
    }
    const std::uint8_t* body = data + *signature;

    Ticket ticket;
    ticket.signature_type = type;
    ticket.issuer = ReadIssuer(body + kTicketIssuer);
    std::memcpy(ticket.title_key.data(), body + kTicketTitleKey, ticket.title_key.size());
    ticket.ticket_id = Read64(body + kTicketId);
    ticket.console_id = Read32(body + kTicketConsoleId);
    ticket.title_id = Read64(body + kTicketTitleId);
    ticket.title_version = Read16(body + kTicketTitleVersion);
    ticket.common_key_index = body[kTicketCommonKeyIndex];
    ticket.raw.assign(data, data + *signature + kBodySize);
    return ticket;
}

std::optional<Tmd> Tmd::Parse(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return std::nullopt;
    }
    const auto type = static_cast<SignatureType>(Read32(data));
    const auto signature = SignatureSize(type);
    if (!signature || size < *signature + kTmdContents) {
        return std::nullopt;
    }
    const std::uint8_t* body = data + *signature;

    Tmd tmd;
    tmd.signature_type = type;
    tmd.issuer = ReadIssuer(body + kTmdIssuer);
    tmd.version = body[kTmdVersion];
    tmd.system_version = Read64(body + kTmdSystemVersion);
    tmd.title_id = Read64(body + kTmdTitleId);
    tmd.title_type = Read32(body + kTmdTitleType);
    tmd.group_id = Read16(body + kTmdGroupId);
    tmd.region = Read16(body + kTmdRegion);
    tmd.access_rights = Read32(body + kTmdAccessRights);
    tmd.title_version = Read16(body + kTmdTitleVersion);
    tmd.boot_index = Read16(body + kTmdBootIndex);

    const std::size_t count = Read16(body + kTmdContentCount);
    const std::size_t needed = *signature + kTmdContents + count * kContentRecordSize;
    if (size < needed) {
        return std::nullopt;
    }
    tmd.contents.reserve(count);
    for (std::size_t index = 0; index < count; index++) {
        const std::uint8_t* record = body + kTmdContents + index * kContentRecordSize;
        ContentRecord content;
        content.id = Read32(record);
        content.index = Read16(record + 0x04);
        content.type = Read16(record + 0x06);
        content.size = Read64(record + 0x08);
        std::memcpy(content.sha1.data(), record + 0x10, content.sha1.size());
        tmd.contents.push_back(content);
    }
    tmd.raw.assign(data, data + needed);
    return tmd;
}

const ContentRecord* Tmd::Content(std::uint16_t index) const {
    for (const ContentRecord& content : contents) {
        if (content.index == index) {
            return &content;
        }
    }
    return nullptr;
}

std::uint64_t Tmd::TotalSize() const {
    std::uint64_t total = 0;
    for (const ContentRecord& content : contents) {
        total += content.size;
    }
    return total;
}

std::array<std::uint8_t, 16> TitleKeyIv(std::uint64_t title_id) {
    std::array<std::uint8_t, 16> iv{};
    for (int index = 0; index < 8; index++) {
        iv[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(title_id >> (56 - index * 8));
    }
    return iv;
}

std::array<std::uint8_t, 16> ContentIv(std::uint16_t content_index) {
    std::array<std::uint8_t, 16> iv{};
    iv[0] = static_cast<std::uint8_t>(content_index >> 8);
    iv[1] = static_cast<std::uint8_t>(content_index & 0xFF);
    return iv;
}

}  // namespace wiinx::nand
