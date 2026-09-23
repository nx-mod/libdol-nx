# accel/sdk/thp

THP video: `THPVideoDecode`, the SDK call a game makes once per frame per movie.

The decoder is in [format/media](../../../format/media/README.md), working on
bytes. What is here is the part that needs a running game: finding how much of
the frame is readable, resolving the three planes it decodes into, and telling
the graphics layer their textures changed.

That last part matters. The SDK writes its planes through a path the platform
reports as a DMA; writing them directly skips that, and the renderer goes on
drawing a cached frame for one plane and a fresh one for another.
