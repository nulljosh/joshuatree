# Badge log

`landing/badge.svg` (mask source, `landing/badge.png` is its raster
fallback) is the engraved mark above the landing page's closing plate. Per
Joshua: same design (tree, ribbons, JOSHUA / TREE / CO. lettering, desert
scene), modernized a notch further on every MINOR version bump. Enforced by
`tools/checks/badge-refresh-check.sh`.

- 1.3: detail pass, simpler shading. Traced to a vector (blur + threshold
  merges the fine hatching, `potrace -t 40 -a 1.0` drops speckle and
  smooths corners) instead of retouching the raster. Dropped the side
  taglines and fine mountain/scrub texture, which read as noise at page
  size; kept the tree silhouette, both ribbons, and the lettering as bold,
  even-weight shapes. Same 620/900 aspect ratio, same ink-mask technique.
