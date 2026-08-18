#!/bin/sh
# Regenerate a logo inscription path from the vendored Atkinson font.
#
# The inscription is the column of six letters down the right edge of the logo.
# It is set in Atkinson Hyperlegible Mono Bold at 10 units on a 100 unit square,
# centred on x=82, one letter every 12 units from y=27 down to y=87, then
# converted from text to outlines so the asset carries no font dependency.
#
# The font is vendored in fonts/, so this does not depend on what is installed
# on the machine. A private fontconfig is built in the work directory and
# pointed at fonts/ alone, which keeps the system font set out of the way and
# leaves the user's own fontconfig untouched.
#
# Requires: Inkscape.
# Usage:   tools/build-logo-type.sh <slug> <letters> [workdir]
# Example: tools/build-logo-type.sh youros YOUROS
# Output:  logo-src/type-<slug>.path and logo-src/type-source-<slug>.svg

set -eu

cd "$(dirname "$0")/.."

[ $# -ge 2 ] || { echo "usage: $0 <slug> <letters> [workdir]" >&2; exit 2; }

SLUG=$1
LETTERS=$2
WORK=${3:-$PWD/type-build}

FAMILY='Atkinson Hyperlegible Mono'
FONT_DIR=$PWD/fonts
SRC=logo-src/type-source-$SLUG.svg
OUT=logo-src/type-$SLUG.path

# fontconfig wants absolute paths in dir and cachedir, and it fails soft: given
# a path it cannot write, it quietly falls back to whatever else it can see
# rather than reporting an error. Resolve WORK to an absolute path here, and
# check the match below, so a fallback can never reach the outlines unnoticed.
case $WORK in
    /*) ;;
    *) WORK=$PWD/$WORK ;;
esac

[ ${#LETTERS} -eq 6 ] || {
    echo "$0: the inscription is six letters, got ${#LETTERS}" >&2
    exit 2
}

mkdir -p "$WORK" logo-src

# A fontconfig that sees the vendored directory and nothing else. Inkscape
# resolves the family through fontconfig, so this is what makes the outlines
# reproducible on a clean checkout.
cat > "$WORK/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$FONT_DIR</dir>
  <cachedir>$WORK/fc-cache</cachedir>
</fontconfig>
CONF

FONTCONFIG_FILE=$WORK/fonts.conf
export FONTCONFIG_FILE

# Deliberately no include of /etc/fonts/conf.d. On some systems those rules
# pull the system font directories back in, which is exactly what this private
# configuration exists to keep out.
matched=$(fc-match --format='%{file}' "$FAMILY:bold")
case $matched in
    "$FONT_DIR"/AtkinsonHyperlegibleMono-Bold.otf) ;;
    *)
        echo "$0: '$FAMILY' resolved to ${matched:-nothing}," >&2
        echo "  not the vendored fonts/AtkinsonHyperlegibleMono-Bold.otf." >&2
        exit 1
        ;;
esac

# The text source is kept alongside the outlines so the wordmark stays editable
# as type. The outlines are what the logo actually uses.
{
    printf '%s\n' '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">'
    printf '<g font-family="%s" font-weight="bold" font-size="10" fill="#000000" text-anchor="middle">\n' "$FAMILY"
    y=27
    i=1
    while [ $i -le 6 ]; do
        printf '<text x="82" y="%d">%s</text>\n' "$y" \
            "$(printf '%s' "$LETTERS" | cut -c$i)"
        y=$((y + 12))
        i=$((i + 1))
    done
    printf '</g>\n</svg>\n'
} > "$SRC"

inkscape --export-type=svg --export-plain-svg --export-text-to-path \
         --export-filename="$WORK/$SLUG.svg" "$SRC" 2>/dev/null

# Leading space in the pattern so this matches the d attribute and not the id.
grep -o ' d="[^"]*"' "$WORK/$SLUG.svg" | sed 's|^ d="||; s|"$||' > "$OUT"

lines=$(wc -l < "$OUT")
[ "$lines" -eq 6 ] || {
    echo "$0: expected 6 outlines, got $lines. Is the font family resolving?" >&2
    exit 1
}

echo "wrote $SRC $OUT"
