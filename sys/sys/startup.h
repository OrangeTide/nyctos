/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * What a program is handed at launch, per section 20.4.
 *
 * A process receives its namespace as an array of named directory handles
 * and receives nothing else. The loader places this record in the child's
 * memory, the entry point is called with a pointer to it, and the C
 * library consumes the array before main() runs, exactly the way envp is
 * delivered.
 *
 * It is a record rather than a convention because a handle value means
 * nothing outside the table holding it. A child cannot be told what it
 * holds by any means other than being told, and it cannot name anything it
 * was not told about, which is what section 20.3 means by there being no
 * ambient root.
 *
 * This is the shared kernel and userland interface, like sys/op.h, so the
 * names follow the C header of section 26 rather than the project's usual
 * preference against typedefs of structures.
 */

#ifndef _SYS_STARTUP_H_
#define _SYS_STARTUP_H_

#include <sys/op.h>

#include <stdint.h>

#define SYS_STARTUP_VERSION     1u

/*
 * Both bounds are configuration, per section 2.7. A name longer than the
 * maximum is refused at spawn rather than truncated, because a truncated
 * name is a name for something else.
 */
#define SYS_NS_NAME_MAX         16
#define SYS_NS_MAX              8

/*
 * One named handle. The name is for the library's benefit alone: authority
 * is the handle, and nothing the kernel does consults the name. A process
 * that ignores every name it was given loses nothing it could have had.
 */
typedef struct sys_ns_entry {
    char            name[SYS_NS_NAME_MAX];  /* NUL-terminated */
    sys_handle_t    handle;
    uint32_t        type;                   /* SYS_OBJ_*, for validation */
    uint32_t        _reserved;
} sys_ns_entry_t;

/*
 * The record itself. argv and envp are here because section 20.4 says the
 * namespace arrives alongside them and a record that has to grow a field
 * later is a record every loader has to be taught twice. They are zero
 * until something passes arguments.
 *
 * version is checked by the library rather than assumed. A program loaded
 * by a kernel that writes a record it does not understand should refuse to
 * read the array at all, which is cheaper than discovering the disagreement
 * one field at a time.
 */
typedef struct sys_startup {
    uint32_t        version;
    uint32_t        ns_count;
    int32_t         argc;
    uint32_t        _reserved;
    uintptr_t       argv;       /* char ** */
    uintptr_t       envp;       /* char ** */
    uintptr_t       ns;         /* sys_ns_entry_t *, ns_count of them */
} sys_startup_t;

_Static_assert(sizeof(sys_ns_entry_t) == 32,
  "sys_ns_entry_t is an ABI structure and must stay 32 bytes");
_Static_assert(sizeof(sys_startup_t) == 40,
  "sys_startup_t is an ABI structure and must stay 40 bytes");

#endif /* _SYS_STARTUP_H_ */
