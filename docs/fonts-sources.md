# Font sources

Every library face is SIL Open Font License 1.1. Its licence sits next to it in `tools/fonts/` as `<Name>-OFL.txt`. None of them is Helvetica, Garamond, Comic Sans or Papyrus. They are open look-alikes.

| Face | Stands in for | Subset bytes | Fetched from |
|---|---|---|---|
| Inter | Helvetica | 9,760 | https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf |
| EB Garamond | Garamond | 19,856 | https://raw.githubusercontent.com/google/fonts/main/ofl/ebgaramond/EBGaramond%5Bwght%5D.ttf |
| Comic Neue | Comic Sans | 11,644 | https://raw.githubusercontent.com/google/fonts/main/ofl/comicneue/ComicNeue-Regular.ttf |
| Caveat Brush | Papyrus (nearest open brush face; nothing open is close) | 29,480 | https://raw.githubusercontent.com/google/fonts/main/ofl/caveatbrush/CaveatBrush-Regular.ttf |
| Dancing Script | cursive | 16,424 | https://raw.githubusercontent.com/google/fonts/main/ofl/dancingscript/DancingScript%5Bwght%5D.ttf |
| JetBrains Mono | monospace | 8,748 | https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf |
| Source Serif 4 | serif | 12,828 | https://raw.githubusercontent.com/google/fonts/main/ofl/sourceserif4/SourceSerif4%5Bopsz,wght%5D.ttf |
| Source Sans 3 | sans | 9,112 | https://raw.githubusercontent.com/google/fonts/main/ofl/sourcesans3/SourceSans3%5Bwght%5D.ttf |

Each licence came from the same folder's `OFL.txt`. Total added: 117,852 bytes.

Recipe: variable fonts are pinned at weight 400 with `fontTools.varLib.instancer`, then subset to printable ASCII (U+0020 to U+007E) with no hinting and no layout tables. Only the subset files are kept. `python3 tools/gen/gen_ttf_font.py` turns them into `drivers/lib_*_font.h`.

The faces are embedded in the kernel for now. Loading them from disk is a later step.
