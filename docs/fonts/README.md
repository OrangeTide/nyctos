# Vendored fonts

Vendored so the document builds identically on a clean checkout. The build
passes `--ignore-system-fonts`, so only these and Typst's built-ins are visible
and no machine can silently substitute another face.

| Family | Use | Files | Licence |
| --- | --- | --- | --- |
| TeX Gyre Pagella | Body text | `texgyrepagella-*.otf` | GUST Font Licence, `LICENSE-TeXGyre.txt` |
| IBM Plex Mono | Code, captions, diagram labels | `IBMPlexMono-*.ttf` | SIL OFL 1.1, `LICENSE-IBMPlexMono.txt` |
| Atkinson Hyperlegible Mono | Logo inscription only | `AtkinsonHyperlegibleMono-Bold.otf` | SIL OFL 1.1, `LICENSE-AtkinsonHyperlegibleMono.txt` |

The Atkinson face is not used by the document. It is here for
`tools/build-logo-type.sh`, which outlines the six letters down the right edge
of the logo. The logo assets carry those outlines directly and have no font
dependency, so this file is needed only to regenerate them. Bold is the only
weight the logo uses, so it is the only one vendored.

Two builds of this family are in circulation and they are not licensed alike.
The one here is the Google Fonts build, version 2.001, from
`googlefonts/atkinson-hyperlegible-next-mono`, which ships OFL 1.1 and embeds
that licence in the font. The build downloaded from brailleinstitute.org has the
same outlines but embeds a custom notice reading "without derivatives or
alteration", which is not a redistribution grant. Replace this file only from
the Google Fonts source.

These licences cover this directory only. They do not extend to the rest of the
project, which has no licence chosen yet. All of them permit embedding in a PDF
without the document inheriting the licence.
