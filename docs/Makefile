# Nyctos documentation build.
#
#   make        build the PDF and audit it
#   make check  run every document invariant
#   make figs   compile each figure on its own, to isolate a broken one
#   make html   build an HTML rendering of the same document
#   make site   assemble site/, the gh-pages payload (see scripts/deploy-gh-pages.sh)
#   make view   build and open it
#   make clean  remove generated files
#
# design.md --pandoc--> nyctos.typ --typst--> nyctos.pdf
# design.md --pandoc--> nyctos.html
#
# Figures are Typst source included directly by the document rather than
# exported to SVG, so diagram text is real PDF text. Fonts are vendored under
# fonts/ so a clean checkout builds identically.

DOC       := design.md
OUT       := nyctos.pdf

TYP       := nyctos.typ
TEMPLATE  := template.typ
FONTDIR   := fonts

HTML_OUT  := nyctos.html
SITEDIR   := site

# The Lua filter runs between pandoc's reader and its writer, which is the only
# layer where markdown structure and target format are both known. It curls
# apostrophes before Typst can read one as a prime, and turns the "::: index"
# div into a real two-column index with page numbers.
FILTER    := tools/filter.lua
TERMS     := tools/index-terms.txt

FIGDIR    := fig
FIGS      := $(wildcard $(FIGDIR)/*.typ) $(wildcard $(FIGDIR)/lib/*.typ)

PANDOC    := pandoc
TYPST     := typst

# The only faces the document may embed. Anything else means a glyph fell
# through to a fallback. That has happened silently more than once: Atkinson
# lacked the micro sign, and Typst's default second-level list marker is
# U+2023, which Pagella lacks. Both went unnoticed for several commits because
# nothing checked the output.
FONT_ALLOW := ^(TeXGyrePagella|IBMPlexMono)

# A string that appears only in a figure, never in the prose. If figures ever
# revert to embedded images their glyphs become paths and this vanishes from
# the extracted text.
FIG_CANARY := BLOCKED_SENDING

# Sections carry their own numbers in the prose, so Typst heading numbering stays
# off. Headings are not shifted: level 1 is a Part, level 2 a section, level 3 a
# subsection.
PANDOCFLAGS := \
	--from=markdown \
	--to=typst \
	--template=$(TEMPLATE) \
	--lua-filter=$(FILTER) \
	--wrap=preserve \
	--standalone

# --ignore-system-fonts keeps the build reproducible: only the vendored faces
# and Typst's built-ins are visible, so a different machine cannot silently
# substitute something else.
TYPSTFLAGS := --font-path $(FONTDIR) --ignore-system-fonts --root .

# The figures and the index are both Typst-only: figures arrive as raw
# ```{=typst} blocks and the index div is rewritten to a raw Typst block by
# the filter (see tools/filter.lua), so pandoc drops both when the target
# format isn't typst. The HTML rendering is prose-only as a result; it links
# back to the PDF for diagrams and the index.
HTMLFLAGS := \
	--from=markdown \
	--to=html5 \
	--lua-filter=$(FILTER) \
	--wrap=preserve \
	--standalone \
	--toc \
	--metadata title="Nyctos Design Document"

.PHONY: all check view clean figs fontcheck fontaudit figaudit refaudit \
        indexaudit primeaudit html site
.DEFAULT_GOAL := all

# The audit runs on every build, not only on demand. Leaving it to a separate
# target would catch regressions exactly as well as remembering to look did.
all: fontaudit

$(OUT): $(TYP) $(FIGS) | fontcheck
	$(TYPST) compile $(TYPSTFLAGS) $(TYP) $@

# Generated at the project root rather than in a build directory, because
# Typst resolves include and image paths relative to the file it compiles.
# Keeping it here lets design.md reference logo.svg and fig/*.typ with
# ordinary relative paths.
$(TYP): $(DOC) $(TEMPLATE) $(FILTER) $(TERMS)
	$(PANDOC) $(PANDOCFLAGS) $(DOC) -o $@

html: $(HTML_OUT)

$(HTML_OUT): $(DOC) $(FILTER) $(TERMS)
	$(PANDOC) $(HTMLFLAGS) $(DOC) -o $@

# ---------------------------------------------------------------- invariants

check: fontaudit figaudit refaudit indexaudit primeaudit
	@echo "all checks passed"

# Input side: the vendored faces must be present before Typst runs, or it will
# quietly substitute.
fontcheck:
	@test -d $(FONTDIR) || { echo "error: $(FONTDIR)/ missing"; exit 1; }
	@$(TYPST) fonts --font-path $(FONTDIR) --ignore-system-fonts \
	  | grep -q "TeX Gyre Pagella" \
	  || { echo "error: TeX Gyre Pagella not found in $(FONTDIR)/"; exit 1; }

# Output side: nothing but the vendored faces may end up embedded.
fontaudit: $(OUT)
	@command -v pdffonts >/dev/null 2>&1 || { \
	  echo "note: pdffonts unavailable, skipping font audit"; exit 0; }
	@bad=$$(pdffonts $(OUT) 2>/dev/null | tail -n +3 | awk '{print $$1}' \
	  | sed -E 's/^[A-Z]{6}\+//' | grep -vE '$(FONT_ALLOW)' | sort -u); \
	if [ -n "$$bad" ]; then \
	  echo "error: $(OUT) embeds unexpected fonts:"; \
	  echo "$$bad" | sed 's/^/    /'; \
	  echo "a character fell through to a fallback face."; \
	  pages=$$(for p in $$(seq 1 $$(pdfinfo $(OUT) 2>/dev/null | awk '/Pages/{print $$2}')); do \
	      pdffonts -f $$p -l $$p $(OUT) 2>/dev/null | tail -n +3 | awk '{print $$1}' \
	        | sed -E 's/^[A-Z]{6}\+//' | grep -qvE '$(FONT_ALLOW)' && printf '%s ' $$p; \
	    done); \
	  echo "  on page(s): $$pages"; \
	  for p in $$pages; do \
	    printf '  page %s uses: ' $$p; \
	    pdftotext -f $$p -l $$p $(OUT) - 2>/dev/null \
	      | grep -oP '[^\x00-\x7F]' | sort -u | tr -d '\n'; echo; \
	  done; \
	  echo "replace the character, or add its face to FONT_ALLOW if deliberate."; \
	  exit 1; \
	fi
	@echo "fonts ok: $$(pdffonts $(OUT) 2>/dev/null | tail -n +3 \
	  | awk '{print $$1}' | sed -E 's/^[A-Z]{6}\+//' | sort -u | tr '\n' ' ')"

# Diagram text must remain text. If a figure is ever embedded as an image its
# glyphs become paths and stop being searchable, selectable, and readable to
# assistive software.
figaudit: $(OUT)
	@command -v pdftotext >/dev/null 2>&1 || { \
	  echo "note: pdftotext unavailable, skipping figure audit"; exit 0; }
	@n=$$(pdftotext $(OUT) - 2>/dev/null | grep -c '$(FIG_CANARY)'); \
	if [ "$$n" -eq 0 ]; then \
	  echo "error: no figure text found in $(OUT)."; \
	  echo "figures appear to be embedded as images rather than included as Typst."; \
	  exit 1; \
	fi
	@want=$$(grep -c 'include "$(FIGDIR)/' $(DOC)); \
	got=$$(pdftotext $(OUT) - 2>/dev/null | grep -c '^Figure [0-9]'); \
	if [ "$$want" -ne "$$got" ]; then \
	  echo "error: $(DOC) includes $$want figures but $(OUT) rendered $$got."; \
	  echo "a figure was dropped, duplicated, or rendered without a caption."; \
	  exit 1; \
	fi; \
	echo "figure text ok: searchable, $$got figures"

# Every "section N.M" reference must name a section that exists. Renumbering
# happens often enough that this is worth checking mechanically.
#
# Both the plural and the "sections N and M" form are checked. They were not,
# and a reference to a section 9.6 that had not existed for two renumberings
# sat in the C header comments the whole time: "section " with a trailing space
# does not match "sections", so half the references in the document were never
# looked at. Match the word, then take every number that follows it.
refaudit:
	@grep -oiE 'sections?[ ]+[0-9]+(\.[0-9]+)?([ ]+and[ ]+[0-9]+(\.[0-9]+)?)?' $(DOC) \
	  | grep -oE '[0-9]+(\.[0-9]+)?' | sort -u > /tmp/nyctos-refs.$$$$; \
	grep -oE '^\#{2,4} [0-9]+(\.[0-9]+)?' $(DOC) \
	  | sed 's/^\#* //' | sort -u > /tmp/nyctos-defs.$$$$; \
	bad=$$(comm -23 /tmp/nyctos-refs.$$$$ /tmp/nyctos-defs.$$$$); \
	rm -f /tmp/nyctos-refs.$$$$ /tmp/nyctos-defs.$$$$; \
	if [ -n "$$bad" ]; then \
	  echo "error: $(DOC) references sections that do not exist:"; \
	  echo "$$bad" | sed 's/^/    section /'; \
	  exit 1; \
	fi
	@echo "cross-references ok"

# The index is built by the filter, so it fails silently: a missing terms file,
# a pattern that matches nothing, or a filter that was not run all produce an
# empty index rather than an error. Require that most terms earned a place.
indexaudit: $(OUT)
	@command -v pdftotext >/dev/null 2>&1 || { \
	  echo "note: pdftotext unavailable, skipping index audit"; exit 0; }
	@want=$$(grep -vE '^\s*(#|$$)' $(TERMS) | wc -l); \
	got=$$(grep -c '^#nyc-mark(' $(TYP)); \
	uniq=$$(grep -oE '^#nyc-mark\("[^"]+"' $(TYP) | sort -u | wc -l); \
	if [ "$$got" -eq 0 ]; then \
	  echo "error: no index marks in $(TYP); the lua filter did not run."; \
	  exit 1; \
	fi; \
	if [ "$$uniq" -lt $$(( want * 2 / 3 )) ]; then \
	  echo "error: only $$uniq of $$want terms in $(TERMS) earned a place."; \
	  echo "a pattern is probably wrong; Lua patterns are not regexes."; \
	  exit 1; \
	fi; \
	echo "index ok: $$uniq of $$want terms placed, $$got references"

# U+2032 PRIME is the feet-and-minutes mark. Typst reads an apostrophe after a
# digit as one, so "section 8's" rendered as "section 8-prime-s". The filter
# curls apostrophes before Typst sees them; this checks that it worked. The
# font audit cannot catch this, because Pagella has the glyph and renders it
# cleanly wrong.
primeaudit: $(OUT)
	@command -v pdftotext >/dev/null 2>&1 || { \
	  echo "note: pdftotext unavailable, skipping prime audit"; exit 0; }
	@n=$$(pdftotext $(OUT) - 2>/dev/null | grep -c '′'); \
	if [ "$$n" -ne 0 ]; then \
	  echo "error: $(OUT) contains $$n prime characters (U+2032)."; \
	  pdftotext $(OUT) - 2>/dev/null | grep -n '′' | sed 's/^/    /'; \
	  echo "an apostrophe after a digit reached Typst uncurled."; \
	  exit 1; \
	fi; \
	echo "apostrophes ok: no primes"

# ---------------------------------------------------------------- utilities

# Compile each figure alone. The document build reports the first failure
# only, which is unhelpful when a shared helper has broken several at once.
figs:
	@for f in $(wildcard $(FIGDIR)/*.typ); do \
	  printf '%-28s ' "$$f"; \
	  $(TYPST) compile $(TYPSTFLAGS) --format png "$$f" /dev/null 2>&1 \
	    && echo ok || echo FAILED; \
	done

view: $(OUT)
	@xdg-open $(OUT) >/dev/null 2>&1 || open $(OUT) 2>/dev/null || echo "built $(OUT)"

# The gh-pages payload: a landing page plus both renderings, side by side.
# scripts/deploy-gh-pages.sh publishes exactly this directory's contents, so
# it stays a plain copy with no build artifacts of its own beyond index.html.
site: $(OUT) $(HTML_OUT)
	@mkdir -p $(SITEDIR)
	cp $(OUT) $(HTML_OUT) $(SITEDIR)/
	@{ \
	  echo '<!doctype html>'; \
	  echo '<html lang="en"><head><meta charset="utf-8">'; \
	  echo '<title>Nyctos Design Document</title></head><body>'; \
	  echo '<h1>Nyctos Design Document</h1>'; \
	  echo '<p>The design document, generated from'; \
	  echo '<code>docs/design.md</code>:</p>'; \
	  echo '<ul>'; \
	  echo "<li><a href=\"$(HTML_OUT)\">HTML</a> — quick to read online, but"; \
	  echo '    prose only: figures and the index are Typst-only and appear'; \
	  echo '    only in the PDF.</li>'; \
	  echo "<li><a href=\"$(OUT)\">PDF</a> — the complete document, with"; \
	  echo '    figures and index.</li>'; \
	  echo '</ul>'; \
	  echo '</body></html>'; \
	} > $(SITEDIR)/index.html
	@echo "site ok: $(SITEDIR)/"

clean:
	rm -f $(TYP) $(OUT) $(HTML_OUT)
	rm -rf $(SITEDIR)
