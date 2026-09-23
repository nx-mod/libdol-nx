# format/archive

What games pack their files in.

| Format | Is |
|---|---|
| Yaz0 (`SZS`) | the run-length compression every EAD game stores its archives in |
| U8 (`ARC`) | a directory of files in one blob, which the SDK and the channels both use |

The algorithms live here; the guest-facing versions - `EGG::Decomp::decodeSZS`,
`ARCOpen` - are natives in `accel` that call them.

*Yaz0 is in `accel/egg` and moves here; U8 is to write.*
