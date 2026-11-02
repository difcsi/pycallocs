# Standalone liballocs bug reproducers

Minimal C programs — no Lua — for the issues in
[../LIBALLOCS_BUGS.md](../LIBALLOCS_BUGS.md). Each is a few dozen lines
linked against `liballocs_preload.so`.

```sh
./run.sh [LIBALLOCS_HOME]     # builds and runs all five, reports REPRODUCED / NOT-REPRODUCED
```

`LIBALLOCS_HOME` defaults to `../../contrib/liballocs`.

| Bug | Driver | Trigger | Expected |
|---|---|---|---|
| 1 realloc of mmap-served chunk | `bug1_mmap_realloc.c` | `./bug1_mmap_realloc` | SIGSEGV (exit 139) |
| 2 `LIBALLOCS_ERR` startup crash | `query.c` | `LIBALLOCS_ERR=/dev/null ./query` | SIGSEGV, pre-`main` |
| 3 empty-environment startup crash | `query.c` | `env -i ./query` | SIGSEGV, pre-`main` |
| 4 `register_gc_policy` off-by-one | `bug4_policy_oob.c` | `./bug4_policy_oob` | 4th call returns id 4 (OOB) |
| 5 unsilenceable debug output | `query.c` | `LIBALLOCS_DEBUG_LEVEL=0 ./query` | noise on stderr |

## Two drivers, and why

- **`bug1_mmap_realloc.c`** and **`bug4_policy_oob.c`** trip their bugs
  through the specific API path they exercise (a threshold-crossing
  `realloc`; four `__liballocs_register_gc_policy` calls) and need
  nothing else.

- **`query.c`** issues one real typed query
  (`__liballocs_get_alloc_info`) to force the meta-DSO / dlbind
  machinery to load. Bugs 2, 3 and 5 all live on that early
  initialization path, and a bare `malloc`/`free` program does **not**
  load enough of liballocs to reach them — the typed query is what
  makes them observable in a standalone binary (matching what the Lua
  interpreter did when they were first found).

## Notes

- **Bug 2 is ASLR-sensitive.** It reproduces on a normal native run
  but *not* under plain gdb or `setarch -R`, both of which disable
  address randomization. To get a backtrace, start gdb with
  `set disable-randomization off`.
- **Bug 4 exits 0 when the bug is present** (it is a logic check, not a
  crash): the 4th registration returning `id == 4` — a slot one past
  the 4-element `__lifetime_policies` array — is the defect.
- All were reproduced against the liballocs revision and toolchain
  recorded at the top of `../LIBALLOCS_BUGS.md`.
