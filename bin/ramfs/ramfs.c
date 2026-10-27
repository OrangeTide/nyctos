/* ramfs.c : an in-memory filesystem server clients can change.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It keeps its directory tree in its own memory and serves the fs protocol
 * on the channel it was given as SYS_NS_SERVICE. A walk resolves a whole
 * relative path, a read returns file bytes, and a directory read returns
 * entries through an opaque cursor, so a large directory is paged across
 * several reads. Clients can create and unlink entries, and an unlink frees
 * the node's slot immediately.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

#define RAMFS_NAME_MAX   16
#define RAMFS_MAX_NODES  32
#define RAMFS_NONE       0xffffffffu
#define RAMFS_ROOT       0

/*
 * A directory read cursor is opaque to the client, and this is the
 * server's own reading of it: the directory it was minted for in the high
 * bits, the key to resume after in the low bits. A client stores it and
 * sends it back and may not compute one. A cursor whose directory half
 * does not match the connection it arrives on names a position somewhere
 * else, and is refused rather than answered. Keys are never reused, so a
 * cursor names a position and not a snapshot: an entry unchanged across a
 * read keeps its key and so appears exactly once.
 */
#define RAMFS_CURSOR(dir, key)  (((uint64_t)((dir) + 1) << 48) | (uint64_t)(key))
#define RAMFS_CURSOR_DIR(c)     ((uint32_t)(((c) >> 48) - 1))
#define RAMFS_CURSOR_KEY(c)     ((c) & (((uint64_t)1 << 48) - 1))

/*
 * A node is a name under a parent, a key in creation order, and either a
 * directory or a file with bytes. The key is never reused, which is what
 * lets a directory read resume from a position rather than a count once
 * that read exists.
 */
struct node {
    char        name[RAMFS_NAME_MAX];
    uint32_t    parent;             /* a node index, or RAMFS_NONE */
    uint64_t    key;
    const char *data;               /* file bytes, or NULL for a directory */
    uint32_t    len;
    uint8_t     is_dir;
    uint8_t     live;
};

static struct node nodes[RAMFS_MAX_NODES];
static uint64_t next_key;

static const char leaf_bytes[] = "reached by a whole-path walk\n";
static const char afile_bytes[] = "a file, not a directory\n";

static void
setname(char *dst, const char *src)
{
    uint32_t i;

    for (i = 0; i + 1 < RAMFS_NAME_MAX && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Add a node under parent with the next key. Slots free once unlinked. */
static uint32_t
add(uint32_t parent, const char *name, int is_dir, const char *data,
  uint32_t len)
{
    uint32_t i;

    for (i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].live) {
            continue;
        }
        setname(nodes[i].name, name);
        nodes[i].parent = parent;
        nodes[i].key = next_key++;
        nodes[i].data = data;
        nodes[i].len = len;
        nodes[i].is_dir = (uint8_t)(is_dir != 0);
        nodes[i].live = 1;
        return i;
    }
    return RAMFS_MAX_NODES;
}

/*
 * The tree, added in index order so the fixed nodes keep known indices: a
 * root with a subdirectory holding a file, a file beside the subdirectory
 * for the not-a-directory case, and a directory that is paged through. The
 * six entries under it are files that are never read.
 */
static void
ramfs_init(void)
{
    static const char *const kids[] = { "a", "b", "c", "d", "e", "f", };
    uint32_t i, big;

    for (i = 0; i < RAMFS_MAX_NODES; i++) {
        nodes[i].name[0] = '\0';
        nodes[i].live = 0;
    }
    next_key = 0;

    add(RAMFS_NONE, "/", 1, NULL, 0);
    add(RAMFS_ROOT, "sub", 1, NULL, 0);
    add(1, "leaf", 0, leaf_bytes, (uint32_t)(sizeof(leaf_bytes) - 1));
    add(RAMFS_ROOT, "afile", 0, afile_bytes,
      (uint32_t)(sizeof(afile_bytes) - 1));
    big = add(RAMFS_ROOT, "big", 1, NULL, 0);
    for (i = 0; i < 6; i++) {
        add(big, kids[i], 0, NULL, 0);
    }
}

/* The live child of dir named by [name, name + len), or RAMFS_MAX_NODES. */
static uint32_t
child(uint32_t dir, const char *name, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].live || nodes[i].parent != dir) {
            continue;
        }
        if (strlen(nodes[i].name) == len &&
          strncmp(nodes[i].name, name, len) == 0) {
            return i;
        }
    }
    return RAMFS_MAX_NODES;
}

/* The length of a fixed-width name field, which need not be terminated. */
static uint32_t
field_len(const char *field)
{
    uint32_t i;

    for (i = 0; i < FS_NAME_MAX; i++) {
        if (field[i] == '\0') {
            break;
        }
    }
    return i;
}

/*
 * Resolve a whole relative path from a starting node, one component at a
 * time. It resolves the whole path or fails: there is no partial result,
 * because the library sends the whole path at once.
 */
static sys_err_t
resolve(uint32_t start, const char *path, uint32_t plen, uint32_t *out)
{
    uint32_t node = start;
    uint32_t i = 0;

    if (plen == 0) {
        return SYS_ERR_INVALID_ARG;
    }
    while (i < plen) {
        uint32_t s = i;
        uint32_t len;
        uint32_t c;

        while (i < plen && path[i] != '/') {
            i++;
        }
        len = i - s;
        while (i < plen && path[i] == '/') {
            i++;
        }
        if (len == 0) {
            continue;
        }
        if (len == 1 && path[s] == '.') {
            continue;
        }
        if (len == 2 && path[s] == '.' && path[s + 1] == '.') {
            return SYS_ERR_PERM_DENIED;
        }
        if (!nodes[node].is_dir) {
            return SYS_ERR_WRONG_TYPE;
        }
        c = child(node, &path[s], len);
        if (c >= RAMFS_MAX_NODES) {
            return SYS_ERR_NO_ENT;
        }
        node = c;
    }
    *out = node;
    return SYS_OK;
}

static void
do_walk(sys_handle_t chan, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    sys_handle_t conn;
    uint32_t found;

    if (node >= RAMFS_MAX_NODES || !nodes[node].live) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    rep->fr_status = resolve(node, req->fr_name, field_len(req->fr_name),
      &found);
    if (rep->fr_status != SYS_OK) {
        return;
    }
    rep->fr_status = sys_chan_connect(chan, found,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &conn);
    if (rep->fr_status == SYS_OK) {
        reply->handle = conn;
        rep->fr_size = nodes[found].len;
    }
}

static void
do_read(const sys_msg_t *message, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    const struct node *f;
    uint32_t avail, cap;
    uint64_t off;
    void *dst;

    if (node >= RAMFS_MAX_NODES || !nodes[node].live) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    if (nodes[node].is_dir) {
        rep->fr_status = SYS_ERR_WRONG_TYPE;    /* a directory, not a file */
        return;
    }
    f = &nodes[node];
    off = req->fr_offset;
    avail = off < f->len ? f->len - (uint32_t)off : 0;
    if (avail > req->fr_length) {
        avail = req->fr_length;
    }

    /*
     * Into the client's buffer when it supplied one, and inline otherwise.
     * A short read is a success.
     */
    if (message->buf != 0 && message->buf_len > 0) {
        dst = (void *)message->buf;
        cap = message->buf_len;
    } else {
        dst = &reply->inline_data[sizeof(*rep)];
        cap = SYS_IPC_INLINE_BYTES - (uint32_t)sizeof(*rep);
    }
    if (avail > cap) {
        avail = cap;
    }
    if (avail > 0) {
        memcpy(dst, &f->data[off], avail);      /* an empty file has none */
    }
    rep->fr_length = avail;
    rep->fr_status = SYS_OK;
}

/*
 * Pack a directory's entries into the client's buffer, in key order, from
 * the position the cursor names. The read stops at the first entry that
 * does not fit, which is a short read: the cursor it returns resumes
 * there, so a directory larger than one buffer pages to its end rather
 * than stopping at the first buffer-full. A read of something that is not
 * a directory is SYS_ERR_WRONG_TYPE, which is the same answer a walk gives
 * a non-directory in the middle of a path.
 */
static void
do_readdir(const sys_msg_t *message, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_readdir_reply_t *rep)
{
    uint8_t *buf;
    uint32_t cap, used, count;
    uint64_t next;
    struct wire_out ents;

    if (node >= RAMFS_MAX_NODES || !nodes[node].live) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    if (!nodes[node].is_dir) {
        rep->fr_status = SYS_ERR_WRONG_TYPE;
        return;
    }

    /*
     * Zero starts a read; any other value is a position, and a position
     * minted for another directory is refused rather than answered. A
     * connection carries no position, so the cursor is read fresh here and
     * nothing about it is held between requests.
     */
    if (req->fr_offset == 0) {
        next = 0;
    } else {
        if (RAMFS_CURSOR_DIR(req->fr_offset) != node) {
            rep->fr_status = SYS_ERR_WRONG_TYPE;
            return;
        }
        next = RAMFS_CURSOR_KEY(req->fr_offset);
    }

    /*
     * Into the client's buffer when it supplied one, and inline otherwise,
     * exactly as a read chooses. A directory too large
     * for whichever space it lands in is a short read that carries a cursor
     * to resume from, so paging is the same whether the entries go inline
     * or to a buffer.
     */
    if (message->buf != 0 && message->buf_len > 0) {
        buf = (uint8_t *)message->buf;
        cap = message->buf_len;
    } else {
        buf = &reply->inline_data[sizeof(fs_readdir_reply_t)];
        cap = SYS_IPC_INLINE_BYTES - (uint32_t)sizeof(fs_readdir_reply_t);
    }
    wire_out_init(&ents, buf, cap);
    count = 0;

    for (;;) {
        uint32_t best = RAMFS_MAX_NODES;
        uint64_t best_key = 0;
        uint32_t i, namelen, pad, need;

        /* The live child with the smallest key at or past the cursor. */
        for (i = 0; i < RAMFS_MAX_NODES; i++) {
            if (!nodes[i].live || nodes[i].parent != node) {
                continue;
            }
            if (nodes[i].key < next) {
                continue;
            }
            if (best >= RAMFS_MAX_NODES || nodes[i].key < best_key) {
                best = i;
                best_key = nodes[i].key;
            }
        }
        if (best >= RAMFS_MAX_NODES) {
            break;
        }

        namelen = (uint32_t)strlen(nodes[best].name);
        pad = FS_DIRENT_PAD(namelen);
        need = (uint32_t)sizeof(fs_dirent_t) + pad;
        if (need > ents.cap - ents.len) {
            break;              /* a short read: the cursor resumes here */
        }

        /*
         * The entry header, then the name padded to the entry alignment.
         * The writer refuses anything past the buffer, so an entry that
         * would overrun is never half-written.
         */
        wire_put_u32(&ents, nodes[best].is_dir ? FS_KIND_DIR : FS_KIND_FILE);
        wire_put_u32(&ents, namelen);
        wire_put_bytes(&ents, (const uint8_t *)nodes[best].name, namelen);
        wire_put_zeros(&ents, pad - namelen);

        count++;
        next = best_key + 1;
    }
    used = ents.len;

    rep->fr_count = count;
    rep->fr_cursor = RAMFS_CURSOR(node, next);
    rep->fr_status = SYS_OK;

    /*
     * The entries sit after the reply header when they went inline, and
     * the reply's inline length must cover them. A buffered read leaves
     * the entries in the buffer and the inline payload is the header
     * alone.
     */
    if (message->buf != 0 && message->buf_len > 0) {
        reply->inline_len = sizeof(fs_readdir_reply_t);
    } else {
        reply->inline_len = (uint32_t)sizeof(fs_readdir_reply_t) + used;
    }
}

/*
 * A create and an unlink name a single entry in the directory the
 * connection names, not a path. The one-component rule is what keeps the
 * subtree backstop automatic here: a single component
 * carries no `..` and crosses no `/`, so there is nothing to escape, and a
 * client that wants to act deeper walks to the parent first. The length is
 * returned, or zero when the name is empty, too long, or not one plain
 * component.
 */
static uint32_t
component_len(const char *field)
{
    uint32_t len = field_len(field);
    uint32_t i;

    if (len == 0 || len >= RAMFS_NAME_MAX) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (field[i] == '/') {
            return 0;
        }
    }
    if (len == 1 && field[0] == '.') {
        return 0;
    }
    if (len == 2 && field[0] == '.' && field[1] == '.') {
        return 0;
    }
    return len;
}

/* Whether any live node still has this node as its parent. */
static int
has_children(uint32_t node)
{
    uint32_t i;

    for (i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].live && nodes[i].parent == node) {
            return 1;
        }
    }
    return 0;
}

static void
do_create(sys_handle_t chan, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    sys_handle_t conn;
    uint32_t len, slot;
    int is_dir;

    if (node >= RAMFS_MAX_NODES || !nodes[node].live) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    if (!nodes[node].is_dir) {
        rep->fr_status = SYS_ERR_WRONG_TYPE;    /* a create under a file */
        return;
    }
    len = component_len(req->fr_name);
    if (len == 0) {
        rep->fr_status = SYS_ERR_INVALID_ARG;
        return;
    }
    if (child(node, req->fr_name, len) < RAMFS_MAX_NODES) {
        /*
         * The name is taken. The error set has no EEXIST yet, so an
         * invalid request is the honest reading until it grows one.
         */
        rep->fr_status = SYS_ERR_INVALID_ARG;
        return;
    }

    /* fr_length names the kind: a directory for FS_KIND_DIR, else a file. */
    is_dir = (req->fr_length == FS_KIND_DIR);
    slot = add(node, req->fr_name, is_dir, NULL, 0);
    if (slot >= RAMFS_MAX_NODES) {
        rep->fr_status = SYS_ERR_NO_MEMORY;     /* the node pool is full */
        return;
    }

    /*
     * Hand back a connection to the new node, as a walk does, so a client
     * can use what it made at once. If the connection cannot be minted the
     * create is undone, so a failed create leaves no node behind.
     */
    rep->fr_status = sys_chan_connect(chan, slot,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &conn);
    if (rep->fr_status == SYS_OK) {
        reply->handle = conn;
    } else {
        nodes[slot].live = 0;
    }
}

static void
do_unlink(const fs_request_t *req, uint32_t node, fs_reply_t *rep)
{
    uint32_t len, target;

    if (node >= RAMFS_MAX_NODES || !nodes[node].live) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    if (!nodes[node].is_dir) {
        rep->fr_status = SYS_ERR_WRONG_TYPE;    /* unlink is from a directory */
        return;
    }
    len = component_len(req->fr_name);
    if (len == 0) {
        rep->fr_status = SYS_ERR_INVALID_ARG;
        return;
    }
    target = child(node, req->fr_name, len);
    if (target >= RAMFS_MAX_NODES) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    if (nodes[target].is_dir && has_children(target)) {
        /*
         * A non-empty directory would orphan its children, and the node
         * pool could not then reclaim them. The error set has no
         * ENOTEMPTY, so again an invalid request is the honest reading.
         */
        rep->fr_status = SYS_ERR_INVALID_ARG;
        return;
    }

    /*
     * The slot is free to reuse at once. A connection a client still holds
     * to this node now names a node that is not live, and its next request
     * is answered SYS_ERR_NO_ENT: an unlink does not reach across the
     * rendezvous to revoke what was already handed out.
     */
    nodes[target].live = 0;
    rep->fr_status = SYS_OK;
}

int
main(void)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    fs_request_t req;
    fs_reply_t rep;
    fs_readdir_reply_t drep;
    struct wire_in in;
    struct wire_out out;
    uint64_t cookie;
    uint32_t node;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }
    ramfs_init();

    for (;;) {
        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 1;
        }

        wire_in_init(&in, message.inline_data, message.inline_len);
        fs_request_get(&in, &req);
        node = (uint32_t)cookie;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(fs_reply_t);
        memset(&rep, 0, sizeof(rep));
        rep.fr_status = SYS_ERR_NO_ENT;

        switch (req.fr_op) {
        case FS_OP_WALK:
            do_walk(chan, &req, node, &reply, &rep);
            break;
        case FS_OP_READ:
            do_read(&message, &req, node, &reply, &rep);
            reply.inline_len = sizeof(fs_reply_t) + rep.fr_length;
            break;
        case FS_OP_READDIR:
            memset(&drep, 0, sizeof(drep));
            do_readdir(&message, &req, node, &reply, &drep);
            break;
        case FS_OP_CLOSE:
            rep.fr_status = SYS_OK;
            break;
        case FS_OP_CREATE:
            do_create(chan, &req, node, &reply, &rep);
            break;
        case FS_OP_UNLINK:
            do_unlink(&req, node, &rep);
            break;
        default:
            rep.fr_status = SYS_ERR_INVALID_ARG;
            break;
        }

        /*
         * Pack the reply. A directory read answers with its own header and
         * sets its own inline length; every other op answers with the
         * reply header, ahead of any inline read bytes left after it.
         */
        wire_out_init(&out, reply.inline_data, SYS_IPC_INLINE_BYTES);
        if (req.fr_op == FS_OP_READDIR) {
            fs_readdir_reply_put(&out, &drep);
        } else {
            fs_reply_put(&out, &rep);
        }

        /* The same answer where the kernel can read it. */
        reply.status = *(const sys_err_t *)reply.inline_data;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 1;
        }
    }
}
