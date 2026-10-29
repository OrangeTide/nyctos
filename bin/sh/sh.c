/* sh.c : a shell with control flow and quoting over programs it runs.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A command is read, tokenized, and interpreted from the token list. The
 * grammar is a Bourne subset: simple commands separated by ';' or newline,
 * the compound commands 'if', 'while', 'until', 'for' and 'case', and the
 * loop builtins 'break' and 'continue' with an optional count.
 *
 * A word may carry single quotes, double quotes and backslash escapes, and
 * '$name', '${name}', '$?', '$#', '$0'..'$9', '${NN}', '$@' and '$*' which
 * expand to a variable's value, the last status or the positional parameters.
 * The 'set' builtin sets the positional parameters. Single quotes keep their
 * contents literal. Double quotes allow the expansions but keep the spaces
 * inside, so a quoted run stays one word. An unquoted expansion is split on
 * whitespace into separate words, so '$@' and a variable holding spaces each
 * spread across the command. Quoting and expansion are resolved when the word
 * is used, not when it is read, so a word reused across loop passes expands
 * against the current values each time. A leading 'NAME=VALUE' word sets a
 * shell variable, and several may precede a command. The 'for' loop also
 * binds its name.
 *
 * The builtins 'true', 'false' and ':' give a condition a status without a
 * program. Everything else is a program the shell spawns and waits for, with
 * the namespace built as before.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LINE_BYTES  128
#define CMD_BYTES   1024
#define MAX_TOKS    256
#define VAR_MAX     16
#define VAR_NAME    24
#define VAR_VALUE   96
#define ARG_POOL    1024
#define FOR_POOL    512
#define FOR_MAX     32
#define POS_MAX     32
#define POS_POOL    512

enum tok_type {
    TOK_EOF,
    TOK_WORD,
    TOK_SEP,        /* ';' or newline between commands */
    TOK_DSEMI,      /* ';;' ends a case clause */
    TOK_RPAREN,     /* ')' ends a case pattern */
};

struct tok {
    enum tok_type type;
    char *text;     /* for TOK_WORD; NULL otherwise */
};

static char linebuf[LINE_BYTES];
static char buf[CMD_BYTES];     /* the whole command, lines joined by '\n' */
static char tbuf[CMD_BYTES];    /* word text, copied out of buf */
static struct tok toks[MAX_TOKS];
static int tok_n;
static int tp;                  /* the interpreter's position in toks */

static char var_name[VAR_MAX][VAR_NAME];
static char var_value[VAR_MAX][VAR_VALUE];
static int var_n;

static char pos_pool[POS_POOL];         /* the positional parameters, packed */
static char *pos_val[POS_MAX];          /* '$1' is pos_val[0] */
static int pos_n;                       /* '$#' */

static int32_t last_status;     /* '$?' and the status 'last' reports */
static int g_eof;               /* the console closed */
static int g_exit;              /* 'exit' ran; leave the shell */
static int32_t exit_code;
static int g_break;             /* levels of loop left to break */
static int g_continue;          /* levels of loop left to continue */
static int g_skip;              /* walk tokens without running them */
static int g_show_status;       /* a program ran, so a nonzero status prints */

static int exec_list(void);

static int
streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int
name_char(char c, int first)
{
    if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return 1;
    }

    return !first && c >= '0' && c <= '9';
}

static int32_t
to_int(const char *s)
{
    int32_t v = 0;
    int neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }

    return neg ? -v : v;
}

static int
int_str(int32_t v, char *out)
{
    char tmp[12];
    unsigned int u;
    int i = 0, j = 0, neg = 0;

    if (v < 0) {
        neg = 1;
        u = (unsigned int)(-(int64_t)v);
    } else {
        u = (unsigned int)v;
    }
    if (u == 0) {
        tmp[i++] = '0';
    }
    while (u != 0) {
        tmp[i++] = (char)('0' + u % 10);
        u /= 10;
    }
    if (neg) {
        out[j++] = '-';
    }
    while (i > 0) {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';

    return j;
}

static const char *
get_var(const char *name)
{
    int i;

    for (i = 0; i < var_n; i++) {
        if (streq(var_name[i], name)) {
            return var_value[i];
        }
    }

    return "";
}

static void
set_var(const char *name, const char *value)
{
    int i, j;

    for (i = 0; i < var_n; i++) {
        if (streq(var_name[i], name)) {
            break;
        }
    }
    if (i == var_n) {
        if (var_n == VAR_MAX) {
            return;     /* the table is full; a later stage grows it */
        }
        var_n++;
        for (j = 0; j + 1 < VAR_NAME && name[j] != '\0'; j++) {
            var_name[i][j] = name[j];
        }
        var_name[i][j] = '\0';
    }
    for (j = 0; j + 1 < VAR_VALUE && value[j] != '\0'; j++) {
        var_value[i][j] = value[j];
    }
    var_value[i][j] = '\0';
}

/*
 * Whether a word is an assignment, 'NAME=VALUE' with NAME a valid name. The
 * name is read from the raw word, before any expansion, so quotes in the
 * value do not change where the name ends.
 */
static int
is_assign(const char *w)
{
    int i;

    if (!name_char(w[0], 1)) {
        return 0;
    }
    for (i = 1; w[i] != '\0' && w[i] != '='; i++) {
        if (!name_char(w[i], 0)) {
            return 0;
        }
    }

    return w[i] == '=';
}

static int
is_ifs(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/*
 * A positional parameter by number. '$0' is the shell's name, '$1' upward are
 * the parameters set by 'set', and anything past the end is empty.
 */
static const char *
pos_param(int i)
{
    if (i == 0) {
        return "sh";
    }
    if (i >= 1 && i <= pos_n) {
        return pos_val[i - 1];
    }

    return "";
}

/*
 * A word being expanded into one or more fields. Characters are appended to
 * the current field, which is closed into argv at a split point or at the
 * end. When split is zero the whole word stays one field, which is what the
 * contexts that do not split (an assignment value, a case word) want.
 */
struct wb {
    char *pool;
    int n;
    int cap;
    char **argv;
    int argc;
    int max;
    int split;
    int open;           /* a field is being built */
    int full;           /* argv ran out of room */
    char *cur;          /* start of the open field */
};

static void
wb_putc(struct wb *b, char c)
{
    if (!b->open) {
        b->cur = b->pool + b->n;
        b->open = 1;
    }
    if (b->n + 1 < b->cap) {
        b->pool[b->n++] = c;
    }
}

static void
wb_end(struct wb *b)
{
    if (!b->open) {
        return;
    }
    if (b->n + 1 < b->cap) {
        b->pool[b->n++] = '\0';
    }
    if (b->argc < b->max) {
        b->argv[b->argc++] = b->cur;
    } else {
        b->full = 1;
    }
    b->open = 0;
}

/*
 * Append a value. An unquoted value is split on whitespace into separate
 * fields when the builder is splitting; a quoted value keeps its spaces.
 */
static void
wb_val(struct wb *b, const char *v, int dq)
{
    if (b->split && !dq) {
        while (*v != '\0') {
            if (is_ifs(*v)) {
                wb_end(b);
                v++;
            } else {
                wb_putc(b, *v++);
            }
        }
    } else {
        while (*v != '\0') {
            wb_putc(b, *v++);
        }
    }
}

/*
 * Expand '$@' or '$*'. '$@' and an unquoted '$*' give one field per
 * parameter; a quoted '$*' joins them with a space into one field; a
 * builder that is not splitting always joins.
 */
static void
wb_params(struct wb *b, int star, int dq)
{
    int per_param = b->split && !(star && dq);
    int i;

    for (i = 0; i < pos_n; i++) {
        if (per_param) {
            if (i > 0) {
                wb_end(b);
            }
            if (pos_val[i][0] == '\0' && !b->open) {
                b->cur = b->pool + b->n;
                b->open = 1;
            }
            wb_val(b, pos_val[i], 1);
        } else {
            if (i > 0) {
                wb_putc(b, ' ');
            }
            wb_val(b, pos_val[i], 1);
        }
    }
}

/*
 * The shared expansion engine. Single quotes keep their contents literal;
 * double quotes keep the expansions but swallow the quote characters; a
 * backslash escapes the next character, with only '$', '"', '\' and '`'
 * escapable inside double quotes. '$name', '${name}', '$?', '$#', '$0'..'$9',
 * '${NN}', '$@' and '$*' expand against the variables, the last status and
 * the positional parameters. A lone '$' or an unknown name expands to
 * nothing beyond what is literal.
 */
static int
expand_core(const char *src, int split, char *pool, int *pos, int cap,
    char **argv, int argc, int max)
{
    struct wb b;
    int sq = 0, dq = 0;

    b.pool = pool;
    b.n = *pos;
    b.cap = cap;
    b.argv = argv;
    b.argc = argc;
    b.max = max;
    b.split = split;
    b.open = !split;            /* a non-splitting word always has a field */
    b.full = 0;
    b.cur = pool + *pos;

    while (*src != '\0') {
        char c = *src;

        if (sq) {
            if (c == '\'') {
                sq = 0;
            } else {
                wb_putc(&b, c);
            }
            src++;
            continue;
        }
        if (c == '\\') {
            if (dq) {
                char d = src[1];

                if (d == '$' || d == '"' || d == '\\' || d == '`') {
                    wb_putc(&b, d);
                    src += 2;
                } else {
                    wb_putc(&b, '\\');
                    src++;
                }
            } else if (src[1] != '\0') {
                wb_putc(&b, src[1]);
                src += 2;
            } else {
                wb_putc(&b, '\\');
                src++;
            }
            continue;
        }
        if (c == '\'' && !dq) {
            sq = 1;
            if (!b.open) {
                b.cur = b.pool + b.n;
                b.open = 1;
            }
            src++;
            continue;
        }
        if (c == '"') {
            dq = !dq;
            if (!b.open) {
                b.cur = b.pool + b.n;
                b.open = 1;
            }
            src++;
            continue;
        }
        if (c != '$') {
            wb_putc(&b, c);
            src++;
            continue;
        }
        src++;
        c = *src;
        if (c == '?') {
            char num[12];

            src++;
            int_str(last_status, num);
            wb_val(&b, num, dq);
        } else if (c == '#') {
            char num[12];

            src++;
            int_str(pos_n, num);
            wb_val(&b, num, dq);
        } else if (c == '@' || c == '*') {
            src++;
            wb_params(&b, c == '*', dq);
        } else if (c >= '0' && c <= '9') {
            src++;
            wb_val(&b, pos_param(c - '0'), dq);
        } else if (c == '{') {
            src++;
            if (*src >= '0' && *src <= '9') {
                int idx = 0;

                while (*src >= '0' && *src <= '9') {
                    idx = idx * 10 + (*src - '0');
                    src++;
                }
                if (*src == '}') {
                    src++;
                }
                wb_val(&b, pos_param(idx), dq);
            } else {
                char name[VAR_NAME];
                int k = 0;

                while (name_char(*src, k == 0) && k + 1 < VAR_NAME) {
                    name[k++] = *src++;
                }
                name[k] = '\0';
                if (*src == '}') {
                    src++;
                }
                wb_val(&b, get_var(name), dq);
            }
        } else if (name_char(c, 1)) {
            char name[VAR_NAME];
            int k = 0;

            while (name_char(*src, k == 0) && k + 1 < VAR_NAME) {
                name[k++] = *src++;
            }
            name[k] = '\0';
            wb_val(&b, get_var(name), dq);
        } else {
            wb_putc(&b, '$');
        }
    }
    wb_end(&b);
    *pos = b.n;

    return b.full ? -1 : b.argc;
}

/*
 * Expand a word to a single string, used where a word is not split: an
 * assignment value and a case word.
 */
static char *
expand(const char *src, char *pool, int *pos, int cap)
{
    char *out = pool + *pos;
    char *one[1];

    (void)expand_core(src, 0, pool, pos, cap, one, 0, 1);

    return out;
}

/*
 * Expand a word into argv, splitting unquoted expansions into fields. Returns
 * the new argc, or -1 if argv had no room.
 */
static int
expand_word(const char *src, char *pool, int *pos, int cap,
    char **argv, int argc, int max)
{
    return expand_core(src, 1, pool, pos, cap, argv, argc, max);
}

/*
 * Match a glob pattern against a string. '*' matches any run, '?' one
 * character, everything else itself. Used by 'case'.
 */
static int
glob(const char *pat, const char *s)
{
    while (*pat != '\0') {
        if (*pat == '*') {
            pat++;
            if (*pat == '\0') {
                return 1;
            }
            while (*s != '\0') {
                if (glob(pat, s)) {
                    return 1;
                }
                s++;
            }
            return glob(pat, s);
        }
        if (*s == '\0') {
            return 0;
        }
        if (*pat != '?' && *pat != *s) {
            return 0;
        }
        pat++;
        s++;
    }

    return *s == '\0';
}

/*
 * A case pattern word may carry '|' alternatives. Match any of them.
 */
static int
glob_any(const char *pat, const char *s)
{
    char one[VAR_VALUE];

    for (;;) {
        int k = 0;

        while (*pat != '\0' && *pat != '|' && k + 1 < (int)sizeof(one)) {
            one[k++] = *pat++;
        }
        one[k] = '\0';
        if (glob(one, s)) {
            return 1;
        }
        if (*pat != '|') {
            return 0;
        }
        pat++;
    }
}

static void
add_tok(enum tok_type type, char *text)
{
    if (tok_n < MAX_TOKS) {
        toks[tok_n].type = type;
        toks[tok_n].text = text;
        tok_n++;
    }
}

/*
 * Split the command buffer into tokens. Words are copied into tbuf so the
 * operators in buf stay readable. ';;' is one token, ';' and newline are a
 * separator, ')' ends a pattern, '(' before a pattern is ignored.
 */
static void
tokenize(void)
{
    const char *s = buf;
    int ti = 0;

    tok_n = 0;
    while (*s != '\0') {
        char c = *s;

        if (c == ' ' || c == '\t') {
            s++;
            continue;
        }
        if (c == ';') {
            if (s[1] == ';') {
                add_tok(TOK_DSEMI, NULL);
                s += 2;
            } else {
                add_tok(TOK_SEP, NULL);
                s++;
            }
            continue;
        }
        if (c == '\n') {
            add_tok(TOK_SEP, NULL);
            s++;
            continue;
        }
        if (c == ')') {
            add_tok(TOK_RPAREN, NULL);
            s++;
            continue;
        }
        if (c == '(') {
            s++;
            continue;
        }
        {
            char *w = tbuf + ti;
            int sq = 0, dq = 0; /* quoting, so a space inside stays in */

            while (*s != '\0') {
                char wc = *s;

                if (sq) {
                    if (ti + 1 < (int)sizeof(tbuf)) {
                        tbuf[ti++] = wc;
                    }
                    s++;
                    if (wc == '\'') {
                        sq = 0;
                    }
                    continue;
                }
                if (wc == '\\') {
                    if (ti + 1 < (int)sizeof(tbuf)) {
                        tbuf[ti++] = wc;
                    }
                    s++;
                    if (*s != '\0') {
                        if (ti + 1 < (int)sizeof(tbuf)) {
                            tbuf[ti++] = *s;
                        }
                        s++;
                    }
                    continue;
                }
                if (wc == '\'' && !dq) {
                    sq = 1;
                } else if (wc == '"') {
                    dq = !dq;
                } else if (!dq && (wc == ' ' || wc == '\t' || wc == ';' ||
                  wc == ')' || wc == '(' || wc == '\n')) {
                    break;
                }
                if (ti + 1 < (int)sizeof(tbuf)) {
                    tbuf[ti++] = wc;
                }
                s++;
            }
            if (ti + 1 < (int)sizeof(tbuf)) {
                tbuf[ti++] = '\0';
            }
            add_tok(TOK_WORD, w);
        }
    }
    add_tok(TOK_EOF, NULL);
}

static int
word_is(int at, const char *s)
{
    return toks[at].type == TOK_WORD && streq(toks[at].text, s);
}

/*
 * A reserved word that ends a command list: the interpreter stops a list
 * when it reaches one at command position, so the compound that opened the
 * block can read it.
 */
static int
is_terminator(int at)
{
    return word_is(at, "then") || word_is(at, "elif") || word_is(at, "else") ||
      word_is(at, "fi") || word_is(at, "do") || word_is(at, "done") ||
      word_is(at, "esac");
}

/*
 * The count of open compound commands over the whole buffer, so a line left
 * inside one draws another line before the command is run. Reserved words
 * count only at command position.
 */
static int
open_depth(void)
{
    int depth = 0, cmdpos = 1, i;

    for (i = 0; i < tok_n; i++) {
        if (toks[i].type == TOK_SEP || toks[i].type == TOK_DSEMI ||
          toks[i].type == TOK_RPAREN) {
            cmdpos = 1;
            continue;
        }
        if (toks[i].type != TOK_WORD) {
            continue;
        }
        if (cmdpos) {
            const char *w = toks[i].text;

            if (streq(w, "if") || streq(w, "while") || streq(w, "until") ||
              streq(w, "for") || streq(w, "case")) {
                depth++;
            } else if (streq(w, "fi") || streq(w, "done") ||
              streq(w, "esac")) {
                depth--;
            }
            cmdpos = streq(w, "then") || streq(w, "do") || streq(w, "else") ||
              streq(w, "elif");
        } else {
            cmdpos = 0;
        }
    }

    return depth;
}

/*
 * Read one command, drawing more lines while a compound command is still
 * open. Returns zero at end of input with nothing read.
 */
static int
read_command(void)
{
    int len = 0, first = 1;

    buf[0] = '\0';
    for (;;) {
        int n;

        printf(first ? "$ " : "> ");
        n = readline(linebuf, sizeof(linebuf));
        if (n < 0) {
            g_eof = 1;
            return len > 0;
        }
        if (len + n + 2 >= (int)sizeof(buf)) {
            printf("sh: line too long\n");
            return 0;
        }
        {
            int i;

            for (i = 0; i < n; i++) {
                buf[len++] = linebuf[i];
            }
            buf[len++] = '\n';
            buf[len] = '\0';
        }
        tokenize();
        if (open_depth() <= 0) {
            return 1;
        }
        first = 0;
    }
}

static const char *
fault_name(uint32_t kind)
{
    switch (kind) {
    case SYS_FAULT_PAGE:        return "page";
    case SYS_FAULT_ACCESS:      return "access";
    case SYS_FAULT_ALIGN:       return "alignment";
    case SYS_FAULT_INSTRUCTION: return "instruction";
    case SYS_FAULT_ARITH:       return "arithmetic";
    case SYS_FAULT_DEBUG:       return "debug";
    case SYS_FAULT_EXTERNAL:    return "external abort";
    default:                    return "unknown";
    }
}

/*
 * Spawn a program and wait for it. The first word or two choose the child's
 * namespace: 'alone' hands over nothing, 'only NAME' one entry, a bare
 * command everything this shell holds. These are a choice about which array
 * to build, not something the kernel or the library knows.
 */
static sys_err_t
run(int argc, char **argv, int32_t *status)
{
    sys_ns_spec_t ns[SYS_NS_MAX];
    const char *only[1] = { NULL };
    ns_how_t how = NS_SHARE;
    sys_handle_t child, done;
    unsigned int nns = 0;
    uint32_t state;
    sys_err_t err;

    if (argc >= 2 && strncmp(argv[0], "alone", 6) == 0) {
        how = NS_NEW;
        argv++;
        argc--;
    } else if (argc >= 3 && strncmp(argv[0], "only", 5) == 0) {
        how = NS_CLONE;
        only[0] = argv[1];
        argv += 2;
        argc -= 2;
    }

    err = ns_compose(how, only, 1, SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER,
      ns, SYS_NS_MAX, &nns);
    if (err != SYS_OK) {
        return err;
    }

    err = sys_proc_spawn(argv[0], SYS_PRIO_INHERIT, ns, nns,
      (const char *const *)argv, (uint32_t)argc, &child);
    if (err != SYS_OK) {
        while (nns > 0) {
            (void)sys_handle_close(ns[--nns].handle);
        }
        return err;
    }

    if (sys_object_completion(child, &done) != SYS_OK) {
        (void)sys_handle_close(child);
        return SYS_ERR_NO_MEMORY;
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        (void)sys_handle_close(done);
        (void)sys_handle_close(child);
        return SYS_ERR_INTERRUPTED;
    }
    if (sys_proc_status(child, status, &state) != SYS_OK) {
        (void)sys_handle_close(done);
        (void)sys_handle_close(child);
        return SYS_ERR_STALE;
    }

    {
        sys_fault_t fault;

        if (sys_proc_fault(child, &fault) == SYS_OK &&
          fault.kind != SYS_FAULT_NONE) {
            printf("sh: %s: %s fault at 0x%x, pc 0x%x\n", argv[0],
              fault_name(fault.kind), (unsigned int)fault.addr,
              (unsigned int)fault.pc);
        }
    }
    (void)sys_handle_close(done);
    (void)sys_handle_close(child);

    return SYS_OK;
}

/*
 * A simple command: the words up to a separator, expanded, run as a builtin
 * or a program. In skip mode the words are walked but nothing runs.
 */
static void
exec_simple(void)
{
    char pool[ARG_POOL];
    char *argv[SYS_MAX_ARGS + 1];
    int pos = 0, argc = 0, over = 0;

    /*
     * Leading assignments set shell variables. The value is expanded, so it
     * may carry quotes and '$'. In skip mode the words are stepped over and
     * nothing is set.
     */
    while (toks[tp].type == TOK_WORD && is_assign(toks[tp].text)) {
        const char *raw = toks[tp].text;
        int eq = 0;

        while (raw[eq] != '=') {
            eq++;
        }
        if (!g_skip) {
            char name[VAR_NAME];
            char *val;
            int j;

            for (j = 0; j < eq && j + 1 < VAR_NAME; j++) {
                name[j] = raw[j];
            }
            name[j] = '\0';
            val = expand(raw + eq + 1, pool, &pos, (int)sizeof(pool));
            set_var(name, val);
            last_status = 0;
        }
        tp++;
    }

    while (toks[tp].type == TOK_WORD) {
        if (!over) {
            int na = expand_word(toks[tp].text, pool, &pos, (int)sizeof(pool),
              argv, argc, SYS_MAX_ARGS);

            if (na < 0) {
                over = 1;
            } else {
                argc = na;
            }
        }
        tp++;
    }
    argv[argc] = NULL;

    if (g_skip || argc == 0) {
        return;
    }
    if (over) {
        printf("sh: too many arguments\n");
        last_status = -1;
        return;
    }

    if (streq(argv[0], "exit")) {
        g_exit = 1;
        exit_code = argc > 1 ? to_int(argv[1]) : 0;
        return;
    }
    if (streq(argv[0], "last")) {
        printf("%d\n", last_status);
        return;
    }
    if (streq(argv[0], "set")) {
        int i, p = 0;

        pos_n = 0;
        for (i = 1; i < argc && pos_n < POS_MAX; i++) {
            char *d = pos_pool + p;
            int j = 0;

            while (argv[i][j] != '\0' && p + 1 < POS_POOL) {
                pos_pool[p++] = argv[i][j++];
            }
            pos_pool[p++] = '\0';
            pos_val[pos_n++] = d;
        }
        last_status = 0;
        return;
    }
    if (streq(argv[0], "true") || streq(argv[0], ":")) {
        last_status = 0;
        return;
    }
    if (streq(argv[0], "false")) {
        last_status = 1;
        return;
    }
    if (streq(argv[0], "break")) {
        g_break = argc > 1 ? (int)to_int(argv[1]) : 1;
        return;
    }
    if (streq(argv[0], "continue")) {
        g_continue = argc > 1 ? (int)to_int(argv[1]) : 1;
        return;
    }

    {
        int32_t status = 0;

        if (run(argc, argv, &status) != SYS_OK) {
            printf("sh: %s: cannot run it\n", argv[0]);
            last_status = -1;
            return;
        }
        last_status = status;
        g_show_status = 1;
    }
}

/*
 * Walk the rest of the current block to its closer without running it, and
 * step past the closer. Used when a loop stops early.
 */
static void
flush_block(const char *closer)
{
    int save = g_skip;

    g_skip = 1;
    (void)exec_list();
    if (word_is(tp, closer)) {
        tp++;
    }
    g_skip = save;
}

static void
exec_if(void)
{
    int outer = g_skip;
    int taken = 0;
    int cond;

    tp++;                       /* 'if' */
    g_skip = outer;
    (void)exec_list();          /* condition */
    cond = (int)last_status;
    if (word_is(tp, "then")) {
        tp++;
    }
    if (!outer && cond == 0) {
        g_skip = 0;
        (void)exec_list();
        taken = 1;
    } else {
        g_skip = 1;
        (void)exec_list();
    }

    for (;;) {
        if (word_is(tp, "elif")) {
            tp++;
            if (outer || taken) {
                g_skip = 1;
                (void)exec_list();
                if (word_is(tp, "then")) {
                    tp++;
                }
                g_skip = 1;
                (void)exec_list();
            } else {
                g_skip = 0;
                (void)exec_list();
                cond = (int)last_status;
                if (word_is(tp, "then")) {
                    tp++;
                }
                if (cond == 0) {
                    g_skip = 0;
                    (void)exec_list();
                    taken = 1;
                } else {
                    g_skip = 1;
                    (void)exec_list();
                }
            }
        } else if (word_is(tp, "else")) {
            tp++;
            if (outer || taken) {
                g_skip = 1;
                (void)exec_list();
            } else {
                g_skip = 0;
                (void)exec_list();
                taken = 1;
            }
        } else {
            break;
        }
    }
    if (word_is(tp, "fi")) {
        tp++;
    }
    g_skip = outer;
}

static void
exec_while(int until)
{
    int outer = g_skip;
    int cond_start;

    tp++;                       /* 'while' or 'until' */
    cond_start = tp;

    if (outer) {
        g_skip = 1;
        (void)exec_list();      /* condition */
        if (word_is(tp, "do")) {
            tp++;
        }
        (void)exec_list();      /* body */
        if (word_is(tp, "done")) {
            tp++;
        }
        g_skip = outer;
        return;
    }

    for (;;) {
        int cond, run_body;

        tp = cond_start;
        g_skip = 0;
        (void)exec_list();
        cond = (int)last_status;
        if (word_is(tp, "do")) {
            tp++;
        }
        run_body = until ? cond != 0 : cond == 0;
        if (!run_body) {
            flush_block("done");
            break;
        }
        g_skip = 0;
        (void)exec_list();      /* body */
        if (g_exit) {
            flush_block("done");
            break;
        }
        if (g_break) {
            flush_block("done");
            g_break--;
            break;
        }
        if (g_continue) {
            if (g_continue > 1) {
                g_continue--;
                flush_block("done");
                break;
            }
            g_continue = 0;
            continue;
        }
        /* a normal pass leaves tp at 'done'; the next pass resets it */
    }
    g_skip = outer;
}

static void
exec_for(void)
{
    char name[VAR_NAME];
    char pool[FOR_POOL];
    char *vals[FOR_MAX];
    int outer = g_skip;
    int nv = 0, pos = 0, body_start, i, k;

    tp++;                       /* 'for' */
    if (toks[tp].type == TOK_WORD) {
        const char *w = toks[tp].text;

        for (k = 0; k + 1 < VAR_NAME && w[k] != '\0'; k++) {
            name[k] = w[k];
        }
        name[k] = '\0';
        tp++;
    } else {
        name[0] = '\0';
    }
    if (word_is(tp, "in")) {
        tp++;
        while (toks[tp].type == TOK_WORD) {
            if (nv < FOR_MAX) {
                int nn = expand_word(toks[tp].text, pool, &pos,
                  (int)sizeof(pool), vals, nv, FOR_MAX);

                nv = nn < 0 ? FOR_MAX : nn;
            }
            tp++;
        }
    }
    while (toks[tp].type == TOK_SEP) {
        tp++;
    }
    if (word_is(tp, "do")) {
        tp++;
    }
    body_start = tp;

    if (outer || nv == 0) {
        g_skip = 1;
        tp = body_start;
        (void)exec_list();
        if (word_is(tp, "done")) {
            tp++;
        }
        g_skip = outer;
        return;
    }

    for (i = 0; i < nv; i++) {
        set_var(name, vals[i]);
        tp = body_start;
        g_skip = 0;
        (void)exec_list();
        if (g_exit) {
            flush_block("done");
            g_skip = outer;
            return;
        }
        if (g_break) {
            flush_block("done");
            g_break--;
            g_skip = outer;
            return;
        }
        if (g_continue) {
            if (g_continue > 1) {
                g_continue--;
                flush_block("done");
                g_skip = outer;
                return;
            }
            g_continue = 0;
            continue;
        }
    }
    if (word_is(tp, "done")) {
        tp++;
    }
    g_skip = outer;
}

static void
exec_case(void)
{
    char word[VAR_VALUE];
    char pool[VAR_VALUE];
    int outer = g_skip;
    int matched = 0, pos = 0, k;
    char *w;

    tp++;                       /* 'case' */
    w = expand(toks[tp].type == TOK_WORD ? toks[tp].text : "",
      pool, &pos, (int)sizeof(pool));
    for (k = 0; k + 1 < (int)sizeof(word) && w[k] != '\0'; k++) {
        word[k] = w[k];
    }
    word[k] = '\0';
    if (toks[tp].type == TOK_WORD) {
        tp++;
    }
    if (word_is(tp, "in")) {
        tp++;
    }
    while (toks[tp].type == TOK_SEP) {
        tp++;
    }

    while (toks[tp].type != TOK_EOF && !word_is(tp, "esac")) {
        int hit = 0;

        while (toks[tp].type == TOK_WORD) {
            if (!outer && !matched && glob_any(toks[tp].text, word)) {
                hit = 1;
            }
            tp++;
        }
        if (toks[tp].type == TOK_RPAREN) {
            tp++;
        }
        if (!outer && !matched && hit) {
            g_skip = 0;
            (void)exec_list();
            matched = 1;
        } else {
            g_skip = 1;
            (void)exec_list();
            g_skip = outer;
        }
        if (toks[tp].type == TOK_DSEMI) {
            tp++;
            while (toks[tp].type == TOK_SEP) {
                tp++;
            }
        }
    }
    if (word_is(tp, "esac")) {
        tp++;
    }
    g_skip = outer;
}

static void
exec_command(void)
{
    if (toks[tp].type == TOK_WORD) {
        const char *w = toks[tp].text;

        if (streq(w, "if")) {
            exec_if();
            return;
        }
        if (streq(w, "while")) {
            exec_while(0);
            return;
        }
        if (streq(w, "until")) {
            exec_while(1);
            return;
        }
        if (streq(w, "for")) {
            exec_for();
            return;
        }
        if (streq(w, "case")) {
            exec_case();
            return;
        }
    }
    exec_simple();
}

/*
 * A command list: commands separated by ';' or newline, run in turn until a
 * block terminator, a ';;', or the end. Stops early when a break, continue
 * or exit is pending and the list is being run rather than skipped.
 */
static int
exec_list(void)
{
    for (;;) {
        while (toks[tp].type == TOK_SEP) {
            tp++;
        }
        if (toks[tp].type == TOK_EOF || toks[tp].type == TOK_DSEMI ||
          toks[tp].type == TOK_RPAREN) {
            return last_status;
        }
        if (is_terminator(tp)) {
            return last_status;
        }
        exec_command();
        if (g_exit) {
            return last_status;
        }
        if (!g_skip && (g_break || g_continue)) {
            return last_status;
        }
    }
}

int
main(void)
{
    printf("nyctos shell. exit leaves, last reports a status.\n");

    for (;;) {
        if (!read_command()) {
            if (g_eof) {
                printf("sh: the console is gone\n");
                return -1;
            }
            continue;
        }
        if (tok_n <= 1) {       /* only TOK_EOF */
            continue;
        }

        tp = 0;
        g_break = 0;
        g_continue = 0;
        g_skip = 0;
        g_show_status = 0;
        (void)exec_list();

        if (g_exit) {
            return exit_code;
        }
        if (g_show_status && last_status != 0) {
            printf("[%d]\n", last_status);
        }
    }
}
