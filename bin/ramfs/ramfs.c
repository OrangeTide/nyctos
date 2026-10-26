/*
 * ramfs: a filesystem server whose directory a client can change.
 *
 * M23 stage one. It holds its tree in its own memory rather than reading
 * a block store, so a create or an unlink is a change to that memory and
 * nothing beneath it. This stage walks a whole path and reads a file. The
 * create, unlink and directory read the tree is built for arrive in later
 * stages and answer SYS_ERR_NOT_SUPPORTED until they do, which is section
 * 25.3's rule that a server names the operations it cannot yet provide
 * rather than failing them as if they did not exist.
 *
 * It is an ordinary program. It is handed a receive handle for its channel
 * as SYS_NS_SERVICE and creates nothing, per section 24.5. The cookie on a
 * connection is the node the request is about, chosen by this server when
 * it minted the connection.
 *
 * A walk resolves a whole relative path one component at a time. A
 * component under a node that is not a directory is SYS_ERR_WRONG_TYPE,
 * because the name exists and names the wrong kind of thing; a component
 * that is not there is SYS_ERR_NO_ENT; and a `..` is SYS_ERR_PERM_DENIED,
 * the subtree backstop of section 20.3 by the strict route a server with
 * one level and no physical parent links takes.
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
 * for the not-a-directory case, and a directory the later stages page
 * through. The six entries under it are files this stage never reads.
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

    if (node >= RAMFS_MAX_NODES || !nodes[node].live ||
      nodes[node].data == NULL) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }
    f = &nodes[node];
    off = req->fr_offset;
    avail = off < f->len ? f->len - (uint32_t)off : 0;
    if (avail > req->fr_length) {
        avail = req->fr_length;
    }

    /*
     * Into the client's buffer when it supplied one, per section 25.5,
     * and inline otherwise. A short read is a success.
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
    memcpy(dst, &f->data[off], avail);
    rep->fr_length = avail;
    rep->fr_status = SYS_OK;
}

int
main(void)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    fs_request_t *req;
    fs_reply_t *rep;
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

        req = (fs_request_t *)message.inline_data;
        node = (uint32_t)cookie;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (fs_reply_t *)reply.inline_data;
        rep->fr_status = SYS_ERR_NO_ENT;

        switch (req->fr_op) {
        case FS_OP_WALK:
            do_walk(chan, req, node, &reply, rep);
            break;
        case FS_OP_READ:
            do_read(&message, req, node, &reply, rep);
            reply.inline_len = sizeof(*rep) + rep->fr_length;
            break;
        case FS_OP_CLOSE:
            rep->fr_status = SYS_OK;
            break;
        case FS_OP_CREATE:
        case FS_OP_UNLINK:
        case FS_OP_READDIR:
            /*
             * The tree is built for these and they arrive in later M23
             * stages. Named rather than left to the default, so a client
             * learns the limit at the point of use, per section 25.3.
             */
            rep->fr_status = SYS_ERR_NOT_SUPPORTED;
            break;
        default:
            rep->fr_status = SYS_ERR_INVALID_ARG;
            break;
        }

        /* Section 12.4: the same answer where the kernel can read it. */
        reply.status = *(const sys_err_t *)reply.inline_data;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 1;
        }
    }
}
