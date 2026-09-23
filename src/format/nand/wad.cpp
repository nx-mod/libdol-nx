#include "wiinx/format/nand/wad.hpp"

#include "wiinx/format/nand/isfs.hpp"

#include <cstdio>
#include <cstring>

namespace wiinx::nand {
namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kAlignment = 0x40;
constexpr std::uint16_t kTypeInstallable = 0x4973;  // "Is"
constexpr std::uint16_t kTypeBoot2 = 0x6962;        // "ib"

std::size_t Align(std::size_t value) { return (value + kAlignment - 1) & ~(kAlignment - 1); }

std::uint16_t Read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void Append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void Pad(std::vector<std::uint8_t>& out) { out.resize(Align(out.size()), 0); }

// A signed blob says what it is in the issuer its signature carries: the ticket
// signer ends in "-XS<id>", the TMD signer in "-CP<id>".
bool LooksLikeTicket(const std::vector<std::uint8_t>& file, std::size_t offset, std::size_t size) {
    if (offset + size > file.size()) {
        return false;
    }
    const auto ticket = Ticket::Parse(file.data() + offset, size);
    return ticket && ticket->issuer.find("-XS") != std::string::npos;
}

}  // namespace

std::optional<Wad> Wad::Parse(const std::vector<std::uint8_t>& file) {
    if (file.size() < kHeaderSize || Read32(file.data()) != kHeaderSize) {
        return std::nullopt;
    }
    const std::uint16_t type = Read16(file.data() + 0x04);
    if (type != kTypeInstallable && type != kTypeBoot2) {
        return std::nullopt;
    }

    const std::size_t certs_size = Read32(file.data() + 0x08);
    const std::size_t crl_size = Read32(file.data() + 0x0C);
    const std::size_t ticket_size = Read32(file.data() + 0x10);
    const std::size_t tmd_size = Read32(file.data() + 0x14);
    const std::size_t data_size = Read32(file.data() + 0x18);
    const std::size_t footer_size = Read32(file.data() + 0x1C);

    // Sections follow the header in order, each starting on a 64-byte boundary.
    std::size_t offset = Align(kHeaderSize);
    const auto take = [&offset](std::size_t size) {
        const WadSection section{offset, size};
        offset = Align(offset + size);
        return section;
    };

    Wad wad;
    wad.mKind = type == kTypeBoot2 ? WadKind::Boot2 : WadKind::Installable;
    wad.mCerts = take(certs_size);
    wad.mCrl = take(crl_size);

    // The ticket and the TMD come next, in an order that depends on the kind -
    // and the type field is not always honest about which, so the layout is
    // settled by looking: a ticket carries an issuer ending "-XS...", a TMD one
    // ending "-CP...". The two orders put the sections at different offsets,
    // because the sections are different sizes.
    const std::size_t tmd_after_ticket = Align(offset + ticket_size);
    const std::size_t ticket_after_tmd = Align(offset + tmd_size);

    WadSection ticket_section{offset, ticket_size};
    WadSection tmd_section{tmd_after_ticket, tmd_size};
    std::size_t after_both = Align(tmd_after_ticket + tmd_size);
    if (!LooksLikeTicket(file, ticket_section.offset, ticket_section.size) &&
        LooksLikeTicket(file, ticket_after_tmd, ticket_size)) {
        // A boot2 image: its TMD comes first, because that is the order the
        // console's first stage reads them in.
        tmd_section = WadSection{offset, tmd_size};
        ticket_section = WadSection{ticket_after_tmd, ticket_size};
        after_both = Align(ticket_after_tmd + ticket_size);
    }
    offset = after_both;
    if (offset > file.size() || ticket_section.offset + ticket_section.size > file.size() ||
        tmd_section.offset + tmd_section.size > file.size()) {
        return std::nullopt;
    }

    const auto ticket = Ticket::Parse(file.data() + ticket_section.offset, ticket_section.size);
    const auto tmd = Tmd::Parse(file.data() + tmd_section.offset, tmd_section.size);
    if (!ticket || !tmd) {
        return std::nullopt;
    }
    wad.mTicket = *ticket;
    wad.mTmd = *tmd;

    // Contents sit back to back in TMD order, each padded to 64 bytes.
    const std::size_t data_start = offset;
    for (const ContentRecord& content : wad.mTmd.contents) {
        const std::size_t size = static_cast<std::size_t>(content.EncryptedSize());
        if (offset + size > file.size()) {
            return std::nullopt;
        }
        wad.mContents.push_back(WadSection{offset, size});
        offset = Align(offset + size);
    }
    if (data_size != 0 && offset - data_start > Align(data_size)) {
        return std::nullopt;  // the TMD claims more content than the WAD carries
    }

    wad.mFooter = WadSection{offset, footer_size};
    if (offset + footer_size > file.size()) {
        wad.mFooter = WadSection{offset, 0};
    }
    return wad;
}

std::optional<WadSection> Wad::Content(std::uint16_t index) const {
    for (std::size_t position = 0; position < mTmd.contents.size(); position++) {
        if (mTmd.contents[position].index == index && position < mContents.size()) {
            return mContents[position];
        }
    }
    return std::nullopt;
}

std::string Wad::Describe() const {
    const TitleId id{mTmd.title_id};
    std::string text = id.HighHex() + "/" + id.LowHex();
    const std::string code = id.Code();
    if (!code.empty()) {
        text += " (" + code + ")";
    }
    char version[32];
    std::snprintf(version, sizeof(version), " v%u", mTmd.title_version);
    text += version;
    if (mKind == WadKind::Boot2) {
        text += ", boot2";
    }
    return text;
}

std::vector<std::uint8_t> Wad::Build(WadKind kind,
                                     const std::vector<std::uint8_t>& certificates,
                                     const std::vector<std::uint8_t>& ticket,
                                     const std::vector<std::uint8_t>& tmd,
                                     const std::vector<std::vector<std::uint8_t>>& contents,
                                     const std::vector<std::uint8_t>& footer) {
    std::size_t data_size = 0;
    for (const std::vector<std::uint8_t>& content : contents) {
        data_size += Align(content.size());
    }

    std::vector<std::uint8_t> out;
    Append32(out, kHeaderSize);
    const std::uint16_t type = kind == WadKind::Boot2 ? kTypeBoot2 : kTypeInstallable;
    out.push_back(static_cast<std::uint8_t>(type >> 8));
    out.push_back(static_cast<std::uint8_t>(type & 0xFF));
    out.push_back(0);  // version
    out.push_back(0);
    Append32(out, static_cast<std::uint32_t>(certificates.size()));
    Append32(out, 0);  // no revocation list
    Append32(out, static_cast<std::uint32_t>(ticket.size()));
    Append32(out, static_cast<std::uint32_t>(tmd.size()));
    Append32(out, static_cast<std::uint32_t>(data_size));
    Append32(out, static_cast<std::uint32_t>(footer.size()));
    Pad(out);

    const auto section = [&out](const std::vector<std::uint8_t>& bytes) {
        out.insert(out.end(), bytes.begin(), bytes.end());
        Pad(out);
    };
    section(certificates);
    // boot2 keeps its TMD ahead of its ticket; everything else is installable.
    if (kind == WadKind::Boot2) {
        section(tmd);
        section(ticket);
    } else {
        section(ticket);
        section(tmd);
    }
    for (const std::vector<std::uint8_t>& content : contents) {
        section(content);
    }
    if (!footer.empty()) {
        section(footer);
    }
    return out;
}

}  // namespace wiinx::nand
