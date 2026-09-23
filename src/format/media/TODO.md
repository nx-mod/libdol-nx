# TODO - format/media

- [ ] The THP container: the file's header, its frame index, and walking frames
      rather than being handed one
- [ ] THP audio: the track beside the video, which the SDK decodes separately
- [ ] A check against a frame decoded by the original, not only by us
- [ ] A channel's banner art, which is a layout rather than a picture. Reading
      nw4r layouts is a larger job than the names were, but it is a documented
      one, and it pays twice: a launcher can draw a real tile, and the layout
      natives in `accel/nw4r` stop being the only part of a menu we understand.

      Where it is written down, best first:

      - [doldecomp/ogws](https://github.com/doldecomp/ogws) `src/nw4r/lyt/` -
        CC0, and it is not a description of the reader but the reader itself:
        `lyt_layout.cpp` walks a BRLYT, `lyt_pane.cpp`, `lyt_picture.cpp`,
        `lyt_textBox.cpp` and `lyt_window.cpp` are the pane kinds,
        `lyt_material.cpp` the materials and `lyt_animation.cpp` the BRLAN
        beside it. This is already where our `Pane::CalculateMtx` native comes
        from.
      - [BrawlCrate](https://github.com/soopercool101/BrawlCrate) - LGPL-3.0,
        C#, reads and writes both formats. Useful as a second opinion on a
        field nobody documented.
      - [Treeki/LayoutStudio](https://github.com/Treeki/LayoutStudio) - GPL-2.0
        only, so read for behaviour and take nothing: that licence does not
        combine with ours.
      - wiibrew, for the container around it all.
- [ ] Shift-JIS, for a Japanese disc's banner text
