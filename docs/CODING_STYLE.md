# C Coding Style Guide

This document defines the coding style used for all C and assembly source
files in this project. It is a self-contained style guide: you do not need
to read any external references to follow it.

This project is C-only today. C++ is expected to be supported in the
future, so this guide also briefly covers the handful of conventions that
differ for C++ (see [C++ (planned support)](#c-planned-support)).

It is adapted from the FreeBSD kernel style guide,
[style(9)](https://man.freebsd.org/cgi/man.cgi?style(9)), with a small
number of deliberate changes:

- Indentation is **4 spaces**, not a hard tab.
- Continuation lines are indented **2 spaces** from the start of the
  statement they continue, not 4.
- No hard tab characters are used anywhere in source files.
- The default project license is **Apache-2.0** (see [Licensing](#licensing)
  below), instead of BSD-2-Clause.

See [Credits](#credits) for the relationship to FreeBSD's style(9).

## General formatting

- Preferred maximum line width is 80 characters. Exceed it only when a
  slightly longer line is clearly more readable, or when breaking the line
  would break something that is commonly searched for verbatim (for
  example, a diagnostic or panic message).
- Indent with 4 spaces per level. Never use tab characters for
  indentation or alignment.
- When a statement must be wrapped onto additional lines, indent the
  continuation lines by 2 spaces relative to the start of the original
  statement, and put the operator at the end of the line rather than the
  start of the next one.

  ```c
  while (cnt < 20 && this_variable_name_is_too_long &&
    ep != NULL)
      z = a + really + long + statement + that + needs +
        two + lines + to + fit + on + the + page;
  ```

- Do not leave trailing whitespace at the end of a line.
- Braces: for single-statement bodies, either always use braces or never
  use them — be consistent within a function. Opening braces for
  `if`/`else`/`while`/`for`/`switch` bodies go on the same line as the
  keyword; the closing brace lines up with the keyword. `else` and `else
  if` go on the same line as the preceding closing brace.

  ```c
  if (test)
      stmt;
  else if (bar) {
      stmt;
      stmt;
  } else
      stmt;
  ```

- The opening brace of the function body goes on its own line below the
  parameter list. See [Function declarations: return type
  placement](#function-declarations-return-type-placement) for where the
  return type itself goes.
- `for (;;)` is the preferred way to write an infinite loop; do not use
  `while (1)`.
- Use a single space after `if`, `while`, `for`, `return`, and `switch`.
  Function calls have no space between the function name and the opening
  parenthesis. There is no space after `(` or `[`, and no space before `)`
  or `]`. Commas are followed by a space.

  ```c
  error = function(a1, a2);
  if (error != 0)
      exit(error);
  ```

- Unary operators are not followed by a space; binary operators are
  surrounded by spaces. Only use parentheses for grouping when they are
  required for precedence, or when the expression is genuinely confusing
  without them.
- `switch` bodies are indented; `case` labels are not indented an extra
  level beyond the `switch`. Add a `/* FALLTHROUGH */` comment on any case
  that intentionally falls through to the next one.

  ```c
  switch (ch) {
  case 'a':
      aflag = 1;
      /* FALLTHROUGH */
  case 'b':
      bflag = 1;
      break;
  default:
      usage();
  }
  ```

## Function declarations: return type placement

C and C++ use different return-type placement conventions in this
project, matching how each language is idiomatically written.

**C (prefix return types):** the return type always goes on its own line,
above the function name. This keeps the function name anchored at the
start of a line, which is easier to `grep` for, and keeps complex
pointer/struct return types from crowding the parameter list.

```c
static unsigned long long
calculate_total(int items, int price)
{
    return items * price;
}
```

**C++ (trailing return types):** when using trailing return type syntax
(`auto ... -> Type`) or return type deduction, keep the whole declaration
on one line. `auto` is short, so splitting the line is visually awkward
and rarely needed.

```cpp
auto calculate_total(int items, int price) -> unsigned long long
{
    return items * price;
}
```

This project has no C++ sources yet; the rule above takes effect once C++
files are introduced (see [C++ (planned support)](#c-planned-support)).

## Comments

- "Important" or multi-line comments use the block form, written as real
  sentences and filled like a paragraph:

  ```c
  /*
   * Multi-line comments look like this. Make them real sentences.
   * Fill them so they look like real paragraphs.
   */
  ```

- Short, single-line comments may use either form, but pick one and be
  consistent within a file:

  ```c
  /* Most single-line comments look like this. */
  // Although this style is also acceptable.
  ```

- Comment on *why* code does something, or on anything non-obvious, not on
  what a straightforward line of code already shows.

## Declarations

- Prefer the `uintXX_t` / `intXX_t` fixed-width integer types from
  `<stdint.h>` over other spellings.
- Use `bool` from `<stdbool.h>`, with `true`/`false`, rather than plain
  `int` for boolean values.
- Use `internal_underscores` for identifier names, not `camelCase` or
  `TitleCase`.
- In pointer declarations, do not put whitespace between `*` and the
  variable name; separate the base type from `*` with a single space:

  ```c
  char *cp;
  struct foo *fp;
  ```

- Declare local variables at the top of a block (they may also be declared
  later, immediately before use, when that is clearly more readable),
  sorted roughly by size and then alphabetically. Group one blank line
  between the declarations and the first statement.
- Avoid `typedef` for structure types; refer to `struct foo` directly so
  that the underlying type is never hidden.
- `NULL` is the only acceptable null pointer constant. Compare pointers
  against it explicitly:

  ```c
  if ((fp = fopen(path, "r")) == NULL)
      err(1, "%s", path);
  ```

- Test integers against `0` explicitly rather than relying on implicit
  truthiness: prefer `if (count != 0)` over `if (count)`.

## Header files (`#include`)

- Group includes in this order, with a blank line between each group:
  1. Local project headers, in double quotes (`"uart.h"`).
  2. Third-party / system headers, in angle brackets (`<stdint.h>`).
- Within a group, sort includes alphabetically unless a header has a
  hard ordering dependency on another (document the dependency with a
  comment when that happens).

## C++ (planned support)

This project is C-only today. This section is intentionally short: it
settles the few conventions that are already known to differ for C++,
ahead of C++ actually landing in the tree. A fuller C++ addendum will be
written when C++ sources are actually added.

- C++ source files use the `.cc` extension; C++ headers use `.hh`.
- Function return-type placement follows the C++ (trailing return types)
  rule in [Function declarations: return type
  placement](#function-declarations-return-type-placement).
- Everything else in this guide — indentation, comments, naming,
  licensing — applies equally to C++ unless a future addendum says
  otherwise.

## Static analysis & standards compliance (MISRA C / AUTOSAR)

This project does not currently mandate full MISRA C:2012 or AUTOSAR
C++14 compliance. It is, however, written to stay friendly to the common
static-analysis tools used to check those standards (for example,
clang-tidy's `misra-*`/`cert-*` checks, Cppcheck `--addon=misra`, PC-lint
Plus, or Polyspace Bug Finder). Several rules already in this guide are
practices those tools commonly flag:

- Fixed-width `intXX_t`/`uintXX_t` types instead of plain `int`/`long`
  (see [Declarations](#declarations)).
- Explicit comparisons against `NULL` and `0` instead of relying on
  implicit truthiness (see [Declarations](#declarations)).
- No K&R-style function definitions.
- One license/copyright convention per file, so provenance-checking tools
  have a single, predictable header to parse (see [Licensing](#licensing)).

If this project adopts a specific MISRA C or AUTOSAR ruleset formally,
that will be documented as an addendum here rather than by silently
changing the rules above.

## Tooling

A [`.clang-format`](../.clang-format) file at the top of the repository
encodes the return-type-placement rule above, along with the 4-space
indent, 2-space continuation, and no-tabs rules:

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ContinuationIndentWidth: 2
UseTab: Never
ColumnLimit: 80
AlwaysBreakAfterReturnType: TopLevelDefinitions
AllowShortFunctionsOnASingleLine: Inline
BreakBeforeBraces: Stroustrup
```

- `AlwaysBreakAfterReturnType: TopLevelDefinitions` pushes classic C
  return types (`int`, `void *`, `struct foo *`, etc.) onto their own
  line.
- `AllowShortFunctionsOnASingleLine: Inline` keeps a short C++ `auto
  foo() -> Type { ... }` trailing-return declaration on one line instead
  of being split.
- `BreakBeforeBraces: Stroustrup` puts the function body's opening brace
  on its own line, while keeping `if`/`while`/`for`/`switch` braces
  attached to the keyword.

Run `clang-format -i <file>` before committing; it is expected to
reproduce the conventions in this document without manual touch-up.

## Licensing

The default license for this project is the **Apache License, Version
2.0**. The full license text lives once, at the top level, in
[`LICENSE`](../LICENSE).

Every source file - C, headers, assembly, and linker scripts alike must
begin with an `SPDX-License-Identifier` line and a short copyright
comment, followed by a blank line. Do not reproduce the full license text
in source files; it lives once in `LICENSE`.

Using a single `/* ... */` block for both lines is is the suggested style:

```c
/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */
```

Shell scripts, Makefiles, and other `#`-comment files use `#` throughout:

```sh
# SPDX-License-Identifier: 0BSD OR Apache-2.0
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
```

Linker scripts (`.ld`) only understand `/* ... */` comments, not `//`, so
both lines go in a single block comment as shown above.

If a specific file is ever licensed differently than the project default,
adjust that file's `SPDX-License-Identifier` and copyright lines
accordingly.

## Credits

This guide is adapted from the FreeBSD project's
[style(9)](https://man.freebsd.org/cgi/man.cgi?style(9)) kernel source
style guide. Most wording and examples above are derived from it, modified
for 4-space indentation, 2-space continuation lines, no hard tabs, and an
Apache-2.0 default license. Consult style(9) itself only if you are
curious about the parts of FreeBSD's guide (structure member alignment,
`queue(3)` macros, C++ conventions, etc.) that this project does not use
and that are therefore not repeated here.
