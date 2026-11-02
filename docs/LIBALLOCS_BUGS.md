# liballocs bugs found while building the Lua VM fork

Issues in liballocs (and librunt) discovered on 2026-08-13 while
integrating the native `LUA_TFOREIGN` support into `contrib/lua` and
running the Lua upstream test suite against a liballocs-linked
interpreter. The Lua suite turned out to be a good stress harness: it
exercises large reallocs, subprocess spawning, empty environments and
output comparison — paths the pycallocs suites never hit.

**Environment**

| | |
|---|---|
| liballocs | `/home/zoltan/Develop/stackscan/contrib/liballocs` @ `e78a96d` ("alaska fixes"), debug-flavoured build of 2026-08-13 |
| glibc | Ubuntu GLIBC 2.43-2ubuntu2.3 |
| kernel | Linux 7.0.0-29-generic, x86-64 |
| reproducers | standalone C programs (no Lua) in [`liballocs-repros/`](liballocs-repros/) — run `liballocs-repros/run.sh`. Originally found via `pycallocs/contrib/lua/lua` built with `LIBALLOCS_CONFIG=1`. |

Severity ordering: bugs 1–3 are crashes, bug 4 is a silent
out-of-bounds write, bug 5 is a usability defect that compounds bug 2.

---

## 1. realloc of an mmap-served chunk crashes / corrupts the malloc index

**Severity: high** — any liballocs-attached process that reallocs a
chunk large enough for glibc to serve it via `mmap` (default threshold
≈128 KiB) can crash or silently corrupt the heap index.

### Reproduction

Lua's stack reallocation does exactly this. With the liballocs-linked
interpreter:

```sh
cd pycallocs/contrib/lua/testes
../lua locals.lua        # SIGSEGV (exit 139)
```

The crash fires in the "errors in `__close`" section, where Lua grows a
stack from 188 392 to 279 032 bytes via `realloc`.

### Backtrace

```
#0 arena_for_userptr (a=&__global_malloc_allocator, userptr=0x7ffff7f84010)
     at include/generic_malloc_index.h:215
#1 arena_for_userptr (...) at include/generic_malloc_index.h:203   <- self-recursive
#2 arena_info_for_userptr (...) at include/generic_malloc_index.h:224
#3 __liballocs_malloc_post_nonnull_nonzero_realloc (userptr=0x7ffff7f84010,
     modified_size=279032, old_usable_size=188392, caller=luaM_realloc_+35, ...)
     at src/allocators/default-lib-malloc.c:41
#4 hook_realloc (...) at libmallochooks/src/hook2event.c:183
#5 luaM_realloc_ ()
```

### Analysis

`0x7ffff7f84010` is an mmap-served glibc chunk. glibc implements
realloc of such chunks with `mremap`, which can move the mapping
without any malloc-hook event that the index understands, so
`arena_for_userptr` is asked about a userptr its arena bookkeeping
does not cover and walks into bad memory.

### Workaround, and its limits

`MALLOC_MMAP_THRESHOLD_=1073741824` (force glibc to serve everything
from the main arena) makes `locals.lua` pass — but only *moves* the
failure: a full testes run then dies later with cumulative heap
corruption:

```
Fatal glibc error: malloc.c:2371 (sysmalloc): assertion failed:
(old_top == initial_top (av) && old_size == 0) || ...
```

i.e. the index/hooks still corrupt the heap somewhere on the
large-allocation path; the mmap threshold just changes where it lands.

This is why the Lua fork's upstream-testes gate links
`liballocs_dummyweaks.so` instead of the full runtime
(`LIBALLOCS_LINK=dummyweaks` in `contrib/lua/makefile`).

---

## 2. `LIBALLOCS_ERR=<path>` segfaults any attached process at startup

**Severity: high** — the documented way to redirect liballocs output
crashes the process before `main`. Any target path reproduces it
(`/dev/null` and regular files alike).

### Reproduction

```sh
LIBALLOCS_ERR=/dev/null ./lua -e 'print("ok")'    # exit 139
LIBALLOCS_ERR=/tmp/x    ./lua -e 'print("ok")'    # exit 139
```

Note: **does not reproduce under plain gdb** — gdb disables address
randomization by default and the crash is ASLR-sensitive. Use
`set disable-randomization off` (or `setarch -R` to confirm the
inverse: with ASLR off it does not crash).

### Backtrace (gdb, randomization on)

```
#0 __printf_buffer (format="%s`%s+%p", ...) at stdio-common/vfprintf-internal.c:601
#1 __vsnprintf_internal / ___snprintf_chk (maxlen=8192)
#4 format_symbolic_address (addr=dlmalloc+420) at src/util.c:14
#5 do_mmap (mapped_addr=0x710d9f40d000, requested_length=5500928, ...,
     caller=dlmalloc+420, reason="mmap") at src/allocators/mmap.c:802
#6 __mmap_allocator_notify_mmap (...) at src/allocators/mmap.c:913
#7 mmap (...) at src/preload.c:250
#8 mmap_alloc (m=&_gm_, nb=5498992) at libsystrap/contrib/dlmalloc.c:3829
   ... (dlmalloc, called from fopen during early init)
```

### Analysis

Setting the variable makes `get_stream_err()` (`src/err.c`) call
`fopen(errvar, "w")` during early initialization. `fopen` allocates
through the private dlmalloc, which mmaps a fresh ~5.5 MiB arena; the
interposed `mmap` fires the `MMAP:` debug trace (`do_mmap`,
`src/allocators/mmap.c:802`, level 0 — see bug 5), which is a
**re-entry into the output path while the error stream is still being
constructed**, and `format_symbolic_address` (`src/util.c:14`) feeds
`snprintf("%s`%s+%p")` a bad string for the early-init caller
symbolization. Whether the bad pointer faults depends on the address
layout, hence the ASLR sensitivity.

Two independent fixes suggest themselves: guard the debug trace against
re-entry while `stream_err` is unset, and make
`format_symbolic_address` robust to failed symbolization during early
init.

---

## 3. Empty environment (`env -i`) segfaults any attached process at startup

**Severity: medium** — breaks anything that spawns attached processes
with a scrubbed environment. Found because Lua's `testes/main.lua`
runs `env -i lua prog` as part of its standalone-interpreter tests.

### Reproduction

```sh
env -i ./lua -e 'print("ok")'      # exit 139 (SIGSEGV), no output
# variant: with the usual static-TLS tunable it aborts instead:
env -i GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288 ./lua ...  # exit 134
```

### Backtrace

```
#0 environ_getenv (name=<opt>, environ=0x7fffffffee58)
     at librunt/include/relf.h:441
#1 __pageindex_init () at src/pageindex.c:397
#3 __mmap_allocator_init () at src/allocators/mmap.c:1003
#5 __liballocs_global_init () at src/init.c:105
#6 call_init / _dl_init            <- constructor, before main
```

### Analysis

`__pageindex_init` (`src/pageindex.c:397`) reads
`LIBALLOCS_DEBUG_LEVEL` via

```c
char **env = get_auxv_environ(get_auxv_via_libc_stack_end());
debug_envvar = environ_getenv("LIBALLOCS_DEBUG_LEVEL", env);
```

With an empty environment the auxv-derived `env` pointer is not a
valid, NULL-terminated environ array, and `environ_getenv`
(librunt `relf.h:441`) walks straight off it. Needs a NULL/bounds
guard on the `get_auxv_environ` result (and arguably `environ_getenv`
should tolerate `environ == NULL`).

---

## 4. `__liballocs_register_gc_policy` off-by-one: OOB write on the 4th registration

**Severity: medium** (silent corruption, low likelihood today —
nothing registers four policies yet).

`src/lifetime_policies.c`:

```c
static unsigned __last_free_lifetime_policy_id = 1;
static struct lifetime_policy __lifetime_policies[LIFETIME_POLICIES];  /* == 4 */

int __liballocs_register_gc_policy(__gc_callback_t addref, __gc_callback_t delref)
{
	int id = __last_free_lifetime_policy_id++;
	if (id > LIFETIME_POLICIES) return -1;       /* <-- off by one */
	__lifetime_policies[id] = ...;               /* id == 4 writes past the end */
```

Ids start at 1 (0 is the manual-deallocation policy), so with
`LIFETIME_POLICIES == 4` the valid dynamic ids are 1–3. The guard
`id > LIFETIME_POLICIES` admits `id == 4`, and
`__lifetime_policies[4]` writes one element past the array. Fix:
`if (id >= LIFETIME_POLICIES) return -1;`.

(Consequence for clients: at most three dynamic policies exist —
which is why the Lua fork registers exactly one process-wide policy
and multiplexes states behind it, rather than one policy per
`lua_State`.)

---

## 5. Debug output is unconditional and cannot be silenced

**Severity: low**, but it compounds bug 2 into "the noise cannot be
turned off at all", and it breaks output-sensitive harnesses (Lua's
`testes/main.lua` compares spawned-interpreter output byte for byte).

Two emitters in the current build ignore both `LIBALLOCS_DEBUG_LEVEL`
and `LIBALLOCS_ERR`:

- `src/pageindex.c:348` — `write_string("lazily mapped a piece of
  pageindex at ...")` goes straight to fd 2 via `write_string`, not
  through `debug_printf`/`stream_err`, so neither the level nor the
  redirection applies.
- `src/allocators/mmap.c` — the `MMAP:` trace runs at level 0 by its
  own admission:

  ```c
  #define TRACE_MMAP_DEBUG_LEVEL 0 /* FIXME: move this up top, default to >0 */
  ```

  and level-0 messages always print.

The natural escape hatch — `LIBALLOCS_ERR=/dev/null` — is bug 2.
Suggested: route the pageindex message through `debug_printf(1, ...)`,
lift `TRACE_MMAP_DEBUG_LEVEL` to ≥1 as the FIXME says, and fix bug 2 so
redirection works.

---

## Summary of impact on the Lua fork

- The forked interpreter's upstream-testes gate links the
  `dummyweaks` stubs instead of the runtime (bugs 1, 2, 3, 5 together
  make a full-runtime testes run impossible: `locals.lua` crashes on
  realloc, `main.lua` on `env -i` and on output comparison).
- The pycallocs 17-test suite runs fine with the full runtime linked:
  it never reallocs past the mmap threshold and controls its own
  output/environment.
- The fork registers a single process-wide GC policy (bug 4).

All findings above were reproduced against the pinned revision listed
at the top; line numbers refer to that checkout.
