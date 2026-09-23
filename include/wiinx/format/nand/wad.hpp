#pragma once

// WAD files: a title, packed as one file.
//
// A WAD is how a title travels outside a console. It carries the certificate
// chain that signed it, the ticket, the TMD and then every content the TMD
// lists, each section padded out to 64 bytes.
//
// There are two kinds, and both are read here:
//
//   "Is"  an installable title - a channel, a WiiWare or Virtual Console game,
//         an IOS. What almost everything means by "a WAD".
//   "ib"  a boot2 image, which carries the same sections in the other order
//         because it is what the console's first stage reads.
//
// Rather than trust the type field about the order, the reader looks at each
// section: a ticket and a TMD are told apart by the issuer their signature
// carries ("...-XS..." signs tickets, "...-CP..." signs TMDs). A WAD written
// either way therefore reads correctly.
//
// Contents stay as they are on disk: encrypted, in whole AES blocks. Getting
// at a content's bytes needs the title key, which needs a common key, and
// neither is here - `Wad::Content` hands out the encrypted extent and
// `ContentIv` (title.hpp) the IV that goes with it.

#include "wiinx/format/nand/title.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::nand {

// A span of the file this WAD was parsed from.
struct WadSection {
    std::size_t offset = 0;
    std::size_t size = 0;

    bool Empty() const { return size == 0; }
};

enum class WadKind {
    Installable,  // "Is": a channel, a game, an IOS
    Boot2,        // "ib": the console's second-stage loader
};

class Wad {
  public:
    // Reads a WAD's structure. Returns nothing when the header does not hold
    // up or a section runs past the end of the file. The file is not copied:
    // sections point into `file`, which must outlive the Wad.
    static std::optional<Wad> Parse(const std::vector<std::uint8_t>& file);

    WadKind Kind() const { return mKind; }
    const Ticket& GetTicket() const { return mTicket; }
    const Tmd& GetTmd() const { return mTmd; }

    WadSection CertificateChain() const { return mCerts; }
    WadSection CertificateRevocation() const { return mCrl; }
    WadSection Footer() const { return mFooter; }

    // Where one content sits in the file, still encrypted. Contents follow one
    // another in TMD order, each padded to 64 bytes.
    std::optional<WadSection> Content(std::uint16_t index) const;

    // What this WAD is, for a list on screen: "00010001/HAFA" and its version.
    std::string Describe() const;

    // Assembles a WAD from sections already in hand. `contents` are encrypted
    // and in the TMD's order; nothing is checked against the TMD beyond their
    // count, so a caller building a title writes the TMD it means.
    static std::vector<std::uint8_t> Build(WadKind kind,
                                           const std::vector<std::uint8_t>& certificates,
                                           const std::vector<std::uint8_t>& ticket,
                                           const std::vector<std::uint8_t>& tmd,
                                           const std::vector<std::vector<std::uint8_t>>& contents,
                                           const std::vector<std::uint8_t>& footer = {});

  private:
    WadKind mKind = WadKind::Installable;
    Ticket mTicket;
    Tmd mTmd;
    WadSection mCerts;
    WadSection mCrl;
    WadSection mFooter;
    std::vector<WadSection> mContents;  // in TMD order
};

}  // namespace wiinx::nand
