// Tickets, TMDs and WADs: built here, read back through the library.
//
// Nothing Nintendo made is involved - the blobs below are assembled by this
// file, with the fields at the offsets the format puts them - so the check is
// that the reader and the format agree, on any machine.
#include "wiinx/format/nand/nand.hpp"

#include <cstdio>
#include <cstring>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

void Put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 8);
    out[at + 1] = static_cast<std::uint8_t>(value);
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int index = 0; index < 4; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (24 - index * 8));
    }
}

void Put64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
    for (int index = 0; index < 8; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (56 - index * 8));
    }
}

void PutText(std::vector<std::uint8_t>& out, std::size_t at, const char* text) {
    std::memcpy(out.data() + at, text, std::strlen(text));
}

// An RSA-2048 signature and its padding: where the structure starts.
constexpr std::size_t kSignature = 0x100 + 0x3C;
constexpr std::uint64_t kTitleId = 0x0001000148414641ull;  // 00010001/HAFA

std::vector<std::uint8_t> MakeTicket() {
    std::vector<std::uint8_t> ticket(kSignature + wiinx::nand::Ticket::kBodySize, 0);
    Put32(ticket, 0, 0x00010001);
    PutText(ticket, kSignature + 0x00, "Root-CA00000001-XS00000003");
    for (int index = 0; index < 16; index++) {
        ticket[kSignature + 0x7F + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(0xA0 + index);
    }
    Put64(ticket, kSignature + 0x90, 0x0001000000000123ull);  // ticket id
    Put32(ticket, kSignature + 0x98, 0x0403AC68);             // console id
    Put64(ticket, kSignature + 0x9C, kTitleId);
    Put16(ticket, kSignature + 0xA6, 5);                      // title version
    ticket[kSignature + 0xB1] = 1;                            // Korean common key
    return ticket;
}

std::vector<std::uint8_t> MakeTmd(std::uint64_t content_size) {
    std::vector<std::uint8_t> tmd(kSignature + 0xA4 + 0x24, 0);
    Put32(tmd, 0, 0x00010001);
    PutText(tmd, kSignature + 0x00, "Root-CA00000001-CP00000004");
    tmd[kSignature + 0x40] = 1;                               // version
    Put64(tmd, kSignature + 0x44, 0x0000000100000038ull);     // wants IOS56
    Put64(tmd, kSignature + 0x4C, kTitleId);
    Put32(tmd, kSignature + 0x54, 0x00000001);                // title type
    Put16(tmd, kSignature + 0x5C, 2);                         // region
    Put16(tmd, kSignature + 0x9C, 5);                         // title version
    Put16(tmd, kSignature + 0x9E, 1);                         // one content
    Put16(tmd, kSignature + 0xA0, 0);                         // boot index
    Put32(tmd, kSignature + 0xA4 + 0x00, 0x0000001B);         // content id
    Put16(tmd, kSignature + 0xA4 + 0x04, 0);                  // index
    Put16(tmd, kSignature + 0xA4 + 0x06, 1);                  // normal
    Put64(tmd, kSignature + 0xA4 + 0x08, content_size);
    return tmd;
}
}  // namespace

int main() {
    using namespace wiinx::nand;
    std::printf("wiinx::nand title check\n\n");

    // Title ids and the paths they make.
    const TitleId id{kTitleId};
    Check("title id splits into its two words",
          id.High() == 0x00010001 && id.Low() == 0x48414641);
    Check("title id reads as a code", id.Code() == "HAFA");
    Check("content path", ContentPath(id) == "/title/00010001/48414641/content");
    Check("content file path",
          ContentFilePath(id, 0x1B) == "/title/00010001/48414641/content/0000001b.app");
    Check("ticket path", TicketPath(id) == "/ticket/00010001/48414641.tik");

    // setting.txt: the same operation both ways.
    const std::string settings = "AREA=EUR\r\nMODEL=RVL-001(EUR)\r\nSERNO=104443000\r\n";
    const auto scrambled = ScrambleSetting(settings);
    Check("setting.txt is obfuscated", scrambled[0] != static_cast<std::uint8_t>(settings[0]));
    Check("setting.txt round-trips", UnscrambleSetting(scrambled) == settings);

    // A ticket.
    const auto ticket_blob = MakeTicket();
    const auto ticket = Ticket::Parse(ticket_blob.data(), ticket_blob.size());
    Check("ticket parses", ticket.has_value());
    if (ticket) {
        Check("ticket issuer", ticket->issuer == "Root-CA00000001-XS00000003");
        Check("ticket title id", ticket->title_id == kTitleId);
        Check("ticket title version", ticket->title_version == 5);
        Check("ticket common key index", ticket->common_key_index == 1);
        Check("ticket title key", ticket->title_key[0] == 0xA0 && ticket->title_key[15] == 0xAF);
    }

    // A TMD, and the content it lists.
    const std::uint64_t content_size = 0x400 - 3;  // not a whole AES block
    const auto tmd_blob = MakeTmd(content_size);
    const auto tmd = Tmd::Parse(tmd_blob.data(), tmd_blob.size());
    Check("tmd parses", tmd.has_value());
    if (tmd) {
        Check("tmd issuer", tmd->issuer == "Root-CA00000001-CP00000004");
        Check("tmd title id", tmd->title_id == kTitleId);
        Check("tmd wants an IOS", tmd->system_version == 0x0000000100000038ull);
        Check("tmd lists one content", tmd->contents.size() == 1);
        Check("tmd boot content is the one", tmd->BootContent() != nullptr);
        Check("content pads to a whole AES block",
              tmd->contents[0].EncryptedSize() == 0x400);
    }

    // The IVs the format defines.
    Check("title key IV is the title id, zero-padded",
          TitleKeyIv(kTitleId)[0] == 0x00 && TitleKeyIv(kTitleId)[3] == 0x01 &&
              TitleKeyIv(kTitleId)[7] == 0x41 && TitleKeyIv(kTitleId)[8] == 0x00);
    Check("content IV is the index, zero-padded",
          ContentIv(0x0102)[0] == 0x01 && ContentIv(0x0102)[1] == 0x02 &&
              ContentIv(0x0102)[2] == 0x00);

    // A WAD, both kinds: built here, read back.
    const std::vector<std::uint8_t> certificates(0x780, 0x11);
    const std::vector<std::uint8_t> content(static_cast<std::size_t>(0x400), 0x22);
    for (const WadKind kind : {WadKind::Installable, WadKind::Boot2}) {
        const auto file = Wad::Build(kind, certificates, ticket_blob, tmd_blob, {content});
        const auto wad = Wad::Parse(file);
        const char* what = kind == WadKind::Boot2 ? "boot2 WAD" : "installable WAD";
        Check(what, wad.has_value());
        if (!wad) {
            continue;
        }
        Check("  kind", wad->Kind() == kind);
        Check("  ticket found whichever order it is in", wad->GetTicket().title_id == kTitleId);
        Check("  tmd found whichever order it is in", wad->GetTmd().title_version == 5);
        Check("  certificate chain", wad->CertificateChain().size == certificates.size());
        const auto extent = wad->Content(0);
        Check("  content extent", extent.has_value() && extent->size == content.size());
        Check("  content bytes", extent && file[extent->offset] == 0x22);
        Check("  describes itself", wad->Describe().find("(HAFA)") != std::string::npos);
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
