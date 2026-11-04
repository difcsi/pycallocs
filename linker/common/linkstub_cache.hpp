#pragma once
#include <string>
#include <vector>

// Content-addressed cache of compiled linkstubs. The auditor JIT-compiles the
// linkstub on every process that loads an init library; a server that forks
// an app process per instance would pay the compiler on each one. The cache
// keys the compiled liblinkstub.so by a hash of everything it was generated
// from, so a second process with the same inputs loads the cached file and
// never runs the compiler.
//
// Location: $LINKPY_CACHE_DIR (empty disables caching), else
// $XDG_CACHE_HOME/linkpy, else $HOME/.cache/linkpy.

// The process environment as it was when the auditor was loaded. The auditor
// lives in its own link-map namespace with its own libc, whose environ keeps
// pointing at the original environment block; a host that reuses that block
// after start-up (servers do, for their process title) would leave getenv
// reading garbage. Nothing here calls setenv: that would rewrite the shared
// block itself, which other runtimes (liballocs) validate.
void linkpy_capture_environment(char **envp);
const char *linkpy_getenv(const char *name);
// The captured environment as a NULL-terminated "K=V" array for a child
// process, without the names listed (e.g. LD_AUDIT for the JIT compiler).
std::vector<std::string> linkpy_child_environment(const std::vector<std::string> &without);

// The cache directory, or "" when caching is disabled/unavailable.
std::string linkstub_cache_dir();

// The whole file as a string, or "" when it cannot be read.
std::string linkstub_cache_read_file(const std::string &path);

// SHA-256 over the material items (each terminated by a NUL), as lower-case
// hex. An item starting with '@' names a file whose contents are hashed.
std::string linkstub_cache_key(const std::vector<std::string> &material);

// The cached liblinkstub.so for `key`, or "" when there is none.
std::string linkstub_cache_lookup(const std::string &key);

// Hold the key's lock while compiling, so processes starting together on a
// cold cache wait for one compile instead of each running the compiler.
// Returns a descriptor to pass to linkstub_cache_unlock, or -1 (no lock).
int linkstub_cache_lock(const std::string &key);
void linkstub_cache_unlock(int fd);

// Store `built_so` (and `source_cpp`, for inspection) under `key`. Returns
// the cached liblinkstub.so path, or "" when the store failed.
std::string linkstub_cache_store(const std::string &key, const std::string &built_so,
                                 const std::string &source_cpp);
