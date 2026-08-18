-- filter.lua -- pandoc Lua filter for the Nyctos document.
--
--   pandoc --lua-filter=tools/filter.lua ...
--
-- A filter runs over the document tree between reading and writing, where the
-- markdown structure is still intact and the target format is already known.
-- That is the only layer where either of these jobs can be done.
--
-- It does two jobs:
--
--   * apostrophes are handed to Typst already curled, so Typst's smart-quote
--     pass cannot turn the ones after a digit into primes;
--   * "::: index" becomes a real two-column index with page numbers, built
--     from tools/index-terms.txt.
--
-- The two passes are returned as separate filter tables so their order is
-- fixed. The index is built first, over text that still reads as plain
-- strings; the apostrophe pass runs second, because it replaces text with raw
-- inlines that pandoc.utils.stringify would no longer see.

local TERMS_FILE = 'tools/index-terms.txt'

-- ---------------------------------------------------------------------------
-- apostrophes
-- ---------------------------------------------------------------------------
--
-- Pandoc's Typst writer deliberately emits an ASCII apostrophe and leaves
-- smart quotes to Typst, which is usually right. Typst's rule for an
-- apostrophe following a digit is that it is a prime, the feet-and-minutes
-- mark, so "section 8's" came out as "section 8-prime-s". That is wrong
-- typography in any face, and it survived a font audit because Pagella has the
-- glyph and rendered it cleanly wrong.
--
-- The reader has already produced U+2019 by this point. Emitting it as a raw
-- inline carries it past the writer's un-smartening, and Typst's smart-quote
-- pass ignores a character that is not an ASCII quote. Single and double
-- quotation marks are untouched: those arrive as Quoted elements, not as text
-- inside a Str, so Typst still curls them correctly.

local APOS = '\u{2019}'

local function fix_apostrophes(elem)
    if not FORMAT:match('typst') then return nil end

    local text = elem.text
    if not text:find(APOS, 1, true) then return nil end

    local out, pos = {}, 1
    while true do
        local s, e = text:find(APOS, pos, true)
        if not s then break end
        if s > pos then
            table.insert(out, pandoc.Str(text:sub(pos, s - 1)))
        end
        table.insert(out, pandoc.RawInline('typst', APOS))
        pos = e + 1
    end
    if pos <= #text then
        table.insert(out, pandoc.Str(text:sub(pos)))
    end
    return out
end

-- ---------------------------------------------------------------------------
-- the term list
-- ---------------------------------------------------------------------------
--
-- Lua patterns have no alternation, so a term's alternates are kept as a list
-- and tried in turn. %f is the frontier pattern, which is how Lua spells a
-- word boundary: without it "fence" matches inside "fences" only by accident
-- and "pulse" matches inside "impulse" on purpose, neither of which is wanted.

local function escape(s)
    return (s:gsub('[%^%$%(%)%%%.%[%]%*%+%-%?]', '%%%1'))
end

local function load_terms()
    local terms = {}
    local fh = io.open(TERMS_FILE, 'r')
    if not fh then
        io.stderr:write('filter.lua: ' .. TERMS_FILE .. ' not found, index will be empty\n')
        return terms
    end
    for line in fh:lines() do
        line = line:gsub('#.*', ''):gsub('^%s+', ''):gsub('%s+$', '')
        if line ~= '' then
            local name, pat = line:match('^(.-)%s*=%s*(.*)$')
            if not name then name, pat = line, nil end

            -- "Name @N" pins section N: it is always kept and is the primary.
            local pin = nil
            local bare, n = name:match('^(.-)%s*@%s*(%d+)$')
            if bare then name, pin = bare, tonumber(n) end

            local alts = {}
            for alt in (pat or name:lower()):gmatch('[^|]+') do
                table.insert(alts, '%f[%w_]' .. escape(alt) .. '%f[^%w_]')
            end
            table.insert(terms, { name = name, alts = alts, pin = pin })
        end
    end
    fh:close()
    return terms
end

local function matches(text, term)
    for _, pattern in ipairs(term.alts) do
        if text:find(pattern) then return true end
    end
    return false
end

-- ---------------------------------------------------------------------------
-- deciding what to index
-- ---------------------------------------------------------------------------
--
-- An entry listing every section says "everywhere", which is no help to
-- somebody looking something up. A section counts only if the term appears
-- there more than once or appears in one of its headings, and no entry keeps
-- more than five references. The heaviest section is where the thing is
-- actually explained, and its page is set bold.
--
-- Two exceptions, both of which a specification needs and prose does not.
--
-- A term occurring in exactly one section of the document earns that section
-- however few times it is said. A term stated once, in the section that
-- defines it, is the single most useful entry an index can carry, and without
-- this the definition-only terms are exactly the ones that fall out.
--
-- A term may also pin a section with "@N" in the terms file, which keeps that
-- section and makes it the primary whatever the counting says. Frequency is a
-- guess at where a thing is explained, and in a specification the author
-- already knows. Pin the section rather than tuning a pattern until the count
-- happens to come out right.

local KEEP = 5

local function score_sections(hits, headings, pin)
    local total = 0
    for _ in pairs(hits) do total = total + 1 end

    local scored = {}
    for section, count in pairs(hits) do
        local score = count + (headings[section] and 10 or 0)
        if score >= 2 or total == 1 or section == pin then
            table.insert(scored, { section = section, score = score })
        end
    end
    table.sort(scored, function(a, b)
        if a.score ~= b.score then return a.score > b.score end
        return a.section < b.section
    end)

    local primary = scored[1] and scored[1].section or nil

    local kept = {}
    for i = 1, math.min(KEEP, #scored) do
        table.insert(kept, scored[i])
    end

    if pin then
        -- The pin outranks the count, but only if the term actually reaches
        -- that section. A pin naming a section the term never occurs in is a
        -- stale entry in the terms file, and inventing the reference would
        -- hide it rather than report it.
        local present, held = false, false
        for _, s in ipairs(scored) do
            if s.section == pin then present = true end
        end
        for _, s in ipairs(kept) do
            if s.section == pin then held = true end
        end
        if present then
            primary = pin
            -- Keeping the pinned section is the whole point, so it displaces
            -- the lightest of the others rather than being trimmed away.
            if not held then
                if #kept >= KEEP then table.remove(kept) end
                table.insert(kept, { section = pin })
            end
        end
    end

    table.sort(kept, function(a, b) return a.section < b.section end)

    local out = {}
    for _, s in ipairs(kept) do table.insert(out, s.section) end
    return out, primary
end

-- ---------------------------------------------------------------------------
-- the index
-- ---------------------------------------------------------------------------
--
-- There is deliberately no Div function. Pandoc runs element filters before
-- the whole-document one, so a Div filter rewriting "::: index" would destroy
-- the div before the pass below could find it, and the index would come out
-- empty with nothing to say why.

local function is_index_div(block)
    return block.t == 'Div' and block.classes and block.classes:includes('index')
end

-- Code blocks are not counted. Section 23 is a dozen pages of C declarations
-- and would otherwise outweigh the prose for every term in the document, so
-- every entry would point at the header rather than at the explanation.
local function countable(block)
    return block.t ~= 'CodeBlock' and not is_index_div(block)
end

-- Sections are "## N. Title", which is level 2; level 1 is a Part divider and
-- ends the section it follows rather than starting one.
local function section_of(block, current)
    if block.t == 'Header' and block.level == 1 then return nil end
    if block.t == 'Header' and block.level == 2 then
        local n = pandoc.utils.stringify(block):match('^(%d+)%.')
        return n and tonumber(n) or nil
    end
    return current
end

local function build_index(doc)
    local has_index = false
    for _, block in ipairs(doc.blocks) do
        if is_index_div(block) then has_index = true break end
    end
    if not has_index then return nil end

    local terms = load_terms()

    -- Pass one: walk the document in order, tracking which numbered section
    -- each block belongs to, and count where every term appears.
    local hits, headings = {}, {}
    for _, term in ipairs(terms) do
        hits[term.name], headings[term.name] = {}, {}
    end

    local section = nil
    for _, block in ipairs(doc.blocks) do
        section = section_of(block, section)
        if section and countable(block) then
            local text = pandoc.utils.stringify(block):lower()
            local is_heading = (block.t == 'Header')
            for _, term in ipairs(terms) do
                if matches(text, term) then
                    hits[term.name][section] = (hits[term.name][section] or 0) + 1
                    if is_heading then headings[term.name][section] = true end
                end
            end
        end
    end

    -- Pass two: decide which sections each term earns.
    local wanted = {}
    for _, term in ipairs(terms) do
        local sections, primary = score_sections(hits[term.name], headings[term.name], term.pin)
        if #sections > 0 then
            wanted[term.name] = { primary = primary, sections = {} }
            for _, s in ipairs(sections) do
                wanted[term.name].sections[s] = true
            end
        end
    end

    -- Pass three: rebuild the block list, dropping a mark in ahead of the
    -- first block of each earned section. Marking every occurrence would give
    -- an entry twenty page numbers long; one mark per earned section keeps the
    -- entry the shape it would have had if it were built by section.
    -- A RawBlock sits between blocks, so nothing is spliced into a paragraph.
    --
    -- The mark goes before its block, except when the block is the section
    -- heading, where it goes after. A heading commonly falls at the top of a
    -- page, and a mark placed ahead of it resolves to the page before: the
    -- index entry for section 20 pointed at the last page of section 19,
    -- which is exactly the sort of error nobody checks by hand.
    local marked = {}
    local blocks = {}
    section = nil
    for _, block in ipairs(doc.blocks) do
        section = section_of(block, section)
        local pending = {}
        if section and countable(block) then
            local text = pandoc.utils.stringify(block):lower()
            for _, term in ipairs(terms) do
                local want = wanted[term.name]
                local key = term.name .. '#' .. section
                if want and want.sections[section] and not marked[key]
                    and matches(text, term) then
                    marked[key] = true
                    table.insert(pending, pandoc.RawBlock('typst', string.format(
                        '#nyc-mark("%s", primary: %s)',
                        term.name:gsub('\\', '\\\\'):gsub('"', '\\"'),
                        tostring(section == want.primary))))
                end
            end
        end

        local after = (block.t == 'Header')
        if not after then
            for _, m in ipairs(pending) do table.insert(blocks, m) end
        end
        table.insert(blocks, block)
        if after then
            for _, m in ipairs(pending) do table.insert(blocks, m) end
        end
    end
    doc.blocks = blocks

    -- Finally, replace the div itself. Typst is the only part of this pipeline
    -- that knows what a page is, so it collects the marks and reports where
    -- each one landed.
    for i, block in ipairs(doc.blocks) do
        if is_index_div(block) then
            doc.blocks[i] = pandoc.RawBlock('typst', '#nyc-index()')
        end
    end

    return doc
end

return {
    { Pandoc = build_index },
    { Str = fix_apostrophes },
}
