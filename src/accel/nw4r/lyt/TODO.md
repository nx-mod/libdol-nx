# TODO - accel/nw4r/lyt

- [ ] The rest of the pane walk: `Pane::Draw`, and the culling around it
- [ ] Text: `TextBox::Draw` is the other half of a menu's cost
- [ ] Materials' GX setup (`Material::SetupGX`): the other per-frame material work
- [ ] `Layout::Animate` and `AnimTransformBasic`'s frame stepping, so a layout's
      whole animate is one native call
- [ ] The 2007 build's `AnimTransformBasic::Animate(u32, Material*)`: TexMap from
      the TPL whole (ogws lyt_texMap.cpp), once a 2007 game can check it
- [ ] Mario Kart Wii's `Pane::Animate` and `AnimateSelf` are not signed from it
      yet: its map does not name its Home Menu copies (nw4hbm), so near-map finds
      two candidates. Refreshing its map (wiinx-refresh-map) names them
- [ ] More builds: five are known to the build table, three are written
