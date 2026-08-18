#!/bin/sh
# Assemble the Nyctos logo from its components.
#
# Components:
#   logo-src/owl.path            owl traced from an Athenian tetradrachm
#                                photograph, produced by tools/build-logo-owl.sh
#   logo-src/type-nyctos.path    N-Y-C-T-O-S outlined from Atkinson Hyperlegible
#
# Outputs:
#   logo.svg                     dark background, white mark
#   logo-adaptive.svg            no background, ink follows prefers-color-scheme
#
# The two wordmarks share every component except the inscription. Only the six
# letters down the right edge differ.

set -eu

cd "$(dirname "$0")/.."

OWL=logo-src/owl.path

# The olive branch is routed in the idiom of a printed circuit board. Every
# trace runs at 0, 45 or 90 degrees: the stem, the leaf stubs and the olive
# stubs all obey that. The stem narrows past the turn the way a trace narrows
# once current has dropped off to the branches. Pads are free-form in real
# layout, so the leaves are pointed pads and the olives are annular rings,
# that is, a pad with a via hole through it.
#
# Structure and proportion follow a real olive branch: a single stem from the
# cut end to the tip, leaves alternating along it and angled forward toward
# the tip, leaves shrinking as the stem narrows, and drupes on short stalks.
#
# The branch is drawn before the owl and clipped against a clearance line that
# follows the owl's back, so it passes behind the bird with a visible gap
# rather than merging into it.

# leaf: a thin stub trace off the stem that flares at 45 degrees into a
# chamfered pad, drawn along +x from its stem end. The stub is what keeps the
# stem readable as a trace instead of the leaves fusing into it.
LEAF='M 0,-0.3 L 2.4,-0.3 L 3.25,-1.15 L 8.2,-1.15 L 11.7,0
      L 8.2,1.15 L 3.25,1.15 L 2.4,0.3 L 0,0.3 Z'

emit_branch() {
    cat <<BRANCH
  <!-- THE PCB OLIVE BRANCH
       Traces on 45 degree steps, stem narrowing past the turn, leaves as
       pointed pads on stubs, olives as annular ring pads. Clipped along the
       owl's back so it reads as passing behind the bird. -->
  <g clip-path="url(#$2-clearance)">
    <!-- stem: one 45 degree run out from behind the owl, then vertical to the
         tip, narrowing at the turn the way a trace narrows past a junction -->
    <g fill="none" stroke="$1" stroke-linecap="round" stroke-linejoin="round">
      <path d="M 32,52 L 14,34" stroke-width="3.2"/>
      <path d="M 14,34 L 14,10" stroke-width="2.2"/>
    </g>
    <g fill="$1" stroke="none">
      <!-- leaves alternate sides down the stem, every axis at 45 degrees,
           shrinking toward the tip -->
      <path d="$LEAF" transform="translate(22,42) rotate(270) scale(1.20)"/>
      <path d="$LEAF" transform="translate(19,39) rotate(180) scale(1.30)"/>
      <path d="$LEAF" transform="translate(14,32) rotate(225) scale(1.15)"/>
      <path d="$LEAF" transform="translate(14,28) rotate(315) scale(1.10)"/>
      <path d="$LEAF" transform="translate(14,24) rotate(225) scale(1.05)"/>
      <path d="$LEAF" transform="translate(14,20) rotate(315) scale(1.00)"/>
      <path d="$LEAF" transform="translate(14,16) rotate(225) scale(0.90)"/>
      <path d="$LEAF" transform="translate(14,12) rotate(315) scale(0.85)"/>
      <path d="$LEAF" transform="translate(14,10) rotate(270) scale(0.75)"/>
      <!-- olives: annular ring pads on short stubs -->
      <path d="M 19,39 L 16,42" stroke="$1" stroke-width="1.2" fill="none"/>
      <path d="M 12.4,43.4 a 2.2,2.2 0 1,0 4.4,0 a 2.2,2.2 0 1,0 -4.4,0 Z
               M 13.75,43.4 a 0.85,0.85 0 1,0 1.7,0 a 0.85,0.85 0 1,0 -1.7,0 Z"
            fill-rule="evenodd"/>
      <path d="M 14,28 L 17,31" stroke="$1" stroke-width="1.1" fill="none"/>
      <path d="M 16.5,32.4 a 1.9,1.9 0 1,0 3.8,0 a 1.9,1.9 0 1,0 -3.8,0 Z
               M 17.65,32.4 a 0.75,0.75 0 1,0 1.5,0 a 0.75,0.75 0 1,0 -1.5,0 Z"
            fill-rule="evenodd"/>
      <path d="M 14,24 L 11,21" stroke="$1" stroke-width="1.0" fill="none"/>
      <path d="M 7.7,19.6 a 1.9,1.9 0 1,0 3.8,0 a 1.9,1.9 0 1,0 -3.8,0 Z
               M 8.85,19.6 a 0.75,0.75 0 1,0 1.5,0 a 0.75,0.75 0 1,0 -1.5,0 Z"
            fill-rule="evenodd"/>
    </g>
  </g>
BRANCH
}

# The clearance line follows the owl's head and back, offset by about two
# units. Everything on the owl side of it is cut from the branch.
CLEARANCE='M 38,-5 L 38,15 L 34,18 L 32,23 L 30,29 L 26,35 L 22,41 L 18,47
           L 14,53 L 11,59 L 8,67 L 8,105 L 105,105 L 105,-5 Z'

emit() {
    ink=$1
    bg=$2
    style=$3
    name=$4
    slug=$5
    type=$6
    spelled=$7
    adaptive=logo-adaptive.svg
    [ "$slug" = nyctos ] || adaptive=logo-$slug-adaptive.svg

    printf '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100%%" height="100%%" role="img" aria-label="%s">\n' "$name"
    printf '  <title>%s</title>\n' "$name"
    [ -n "$style" ] && printf '  %s\n' "$style"
    cat <<PROV

  <!-- COMPONENT PROVENANCE
       Owl   : traced from a photograph of an Athenian tetradrachm (Athens,
               5th century BC), reverse. Source scan: Cleveland Museum of Art,
               accession 1941.296.b, released CC0, via Wikimedia Commons.
               Relief extracted with ImageMagick from the coin's own light and
               shadow, vectorised with potrace. See tools/build-logo-owl.sh.
       Type  : $spelled outlined from Atkinson Hyperlegible Mono Bold
               (Braille Institute), SIL Open Font License 1.1, see
               fonts/LICENSE-AtkinsonHyperlegibleMono.txt.
       Branch: drawn in printed circuit board idiom. Structure and proportion
               follow a real olive branch.
       Composition, branch geometry, eye marks and layout are machine-generated.
       Assembled by tools/build-logo.sh. Edit that, not this file.
  -->
PROV

    if [ -n "$bg" ]; then
        printf '\n  <!-- Background. Use %s for a transparent asset. -->\n' "$adaptive"
        printf '  <rect width="100" height="100" fill="%s"/>\n' "$bg"
    fi

    cat <<CLIP

  <defs>
    <!-- Clears the coin's own eye sockets so the binary wink can sit in them -->
    <mask id="$slug-eyes">
      <rect width="100" height="100" fill="#FFFFFF"/>
      <circle cx="45.60" cy="27.07" r="7.6" fill="#000000"/>
      <circle cx="58.20" cy="27.40" r="6.3" fill="#000000"/>
    </mask>
    <!-- Keeps the branch off the owl, the way a keepout holds copper clear -->
    <clipPath id="$slug-clearance">
      <path clip-rule="evenodd"
            d="M -5,-5 L 105,-5 L 105,105 L -5,105 Z
               $CLEARANCE"/>
    </clipPath>
  </defs>
CLIP

    echo
    emit_branch "$ink" "$slug"

    cat <<OWLHEAD

  <!-- THE ATHENIAN TETRADRACHM OWL, traced from the coin -->
  <g mask="url(#$slug-eyes)">
    <g transform="translate(6.334,13.95) scale(0.086312)">
      <g transform="translate(0,900) scale(0.1,-0.1)" fill="$ink" stroke="none">
        <path d="
OWLHEAD
    cat "$OWL"
    cat <<EYES
"/>
      </g>
    </g>
  </g>

  <!-- THE BINARY WINK -->
  <g stroke="$ink" fill="none" stroke-linecap="round">
    <circle cx="45.60" cy="27.07" r="4.9" stroke-width="1.7"/>
    <circle cx="45.60" cy="27.07" r="1.2" fill="$ink" stroke="none"/>
    <line x1="58.20" y1="23.0" x2="58.20" y2="31.9" stroke-width="2.6"/>
  </g>

  <!-- THE TERMINAL INSCRIPTION -->
  <g fill="$ink">
EYES
    sed 's|^|    <path d="|; s|$|"/>|' "$type"
    printf '  </g>\n</svg>\n'
}

ADAPTIVE_STYLE='<style>svg{color:#0D1117}@media (prefers-color-scheme:dark){svg{color:#FFFFFF}}</style>'

# wordmark: name slug type-path spelled-out primary-file adaptive-file
build() {
    emit '#FFFFFF' '#0D1117' '' "$1" "$2" "$3" "$4" > "$5"
    emit 'currentColor' '' "$ADAPTIVE_STYLE" "$1" "$2" "$3" "$4" > "$6"
    echo "wrote $5 $6"
}

build Nyctos nyctos logo-src/type-nyctos.path N-Y-C-T-O-S \
      logo.svg logo-adaptive.svg
