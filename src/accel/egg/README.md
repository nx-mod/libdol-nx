# accel/egg

Nintendo EAD's framework, as the games built on it link it.

EGG is compiled into each game rather than linked as a versioned library, so
there is no build banner to match against: the natives here are the ones whose
behaviour is the same in every game that carries them, registered under
`kEgg_Any`.

| Native | Is |
|---|---|
| `EGG::Decomp::decodeSZS` | Yaz0, the compression every EAD game stores its archives in |

`decomp.cpp` is the decoder, written against the host interface and testable
anywhere. `egg_bind.cpp` is what registers it for the game being built.

The decoder refuses what the original trusts: a back-reference pointing before
the output, or a run that would write past the declared size, stops the decode
instead of walking through the guest's memory.
