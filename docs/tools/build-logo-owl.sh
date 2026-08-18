#!/bin/sh
# Regenerate the Nyctos owl path from the source coin photograph.
#
# The owl is not drawn. It is traced from a photograph of an Athenian
# tetradrachm (Athens, 5th century BC), reverse. The scan comes from the
# Cleveland Museum of Art, accession 1941.296.b, released CC0, via Wikimedia
# Commons.
#
# The coin is low relief in silver, so the owl cannot be separated from the
# field by brightness. Thresholding only finds specular highlights on the
# ridges. Instead this script keeps the coin's own light and shadow through an
# adaptive threshold, confines it to a hand-placed region around the bird, and
# vectorises the result.
#
# Requires: curl, ImageMagick, potrace.
# Output: owl.path, the raw path data pasted into logo.svg.

set -eu

WORK=${1:-owl-build}
SRC_URL='https://upload.wikimedia.org/wikipedia/commons/5/5a/Greece%2C_Athens%2C_5th_century_BC_-_Tetradrachm-_Owl_%28reverse%29_-_1941.296.b_-_Cleveland_Museum_of_Art.jpg'

# Region around the owl, in the coordinates of the 840x900 working image.
# Traced by hand off the coin, then smoothed so the outline does not read as a
# polygon. Excludes the coin's olive spray, crescent, AOE letters and exergue.
POLY='365,95 400,55 470,35 560,35 630,60 690,110 715,170 710,230 675,275
      620,310 575,330 600,375 625,435 605,500 565,560 515,605 465,640 415,660
      370,690 345,780 300,800 245,795 225,700 150,675 70,625 38,575 55,515
      115,435 175,372 250,320 300,280 335,235 348,160'

mkdir -p "$WORK"
cd "$WORK"

[ -f coin.jpg ] || curl -sS -L -o coin.jpg "$SRC_URL"

# Crop to the owl, drop to grayscale, and settle the working size.
convert coin.jpg -crop 1820x1950+330+500 +repage -colorspace Gray \
    -resize 900x900 -normalize -median 5 base.png

# Region mask. The blur and re-threshold round the hand-placed corners.
convert -size 840x900 xc:black -fill white -stroke white -strokewidth 1 \
    -draw "polygon $POLY" -blur 0x10 -threshold 50% mask.png

# Keep the relief. The adaptive threshold window sets how much of the coin's
# feather texture survives: smaller windows keep more detail and cost
# legibility at small sizes.
convert base.png -blur 0x3 -lat 150x150+6% -negate \
    mask.png -compose multiply -composite \
    -morphology open disk:4 -morphology close disk:4 owl.png

# Negate so the bird is the traced foreground and the relief marks stay holes.
convert owl.png -negate owl.pbm
potrace -s -a 1.3 -t 60 -o owl.svg owl.pbm

sed -n '/<path/,$p' owl.svg \
    | sed 's|^<path d="||; s|"/>$||; s|</g>||; s|</svg>||' \
    | grep -v '^$' > owl.path

echo "wrote $WORK/owl.path"
echo "potrace group transform: translate(0,900) scale(0.1,-0.1) in a 840x900 box"
