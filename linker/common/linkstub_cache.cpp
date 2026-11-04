#include "linkstub_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <unistd.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

extern char **environ;

namespace fs = std::filesystem;

namespace {

// SHA-256 (FIPS 180-4): small enough to carry rather than depend on a crypto
// library for a cache key.
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t buflen = 0;
    uint64_t total = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char *p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16
                   | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++)
        {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
        {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const void *data, size_t n)
    {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        total += n;
        while (n > 0)
        {
            size_t take = std::min(n, sizeof buf - buflen);
            std::memcpy(buf + buflen, p, take);
            buflen += take;
            p += take;
            n -= take;
            if (buflen == sizeof buf)
            {
                block(buf);
                buflen = 0;
            }
        }
    }

    std::string hex()
    {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80, zero = 0;
        update(&pad, 1);
        while (buflen != 56)
            update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++)
            len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++)
            std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

} // namespace

namespace {
// The snapshot lives in static storage: la_version runs before the host's
// runtime (and any heap instrumentation it brings, e.g. liballocs) is up, so
// it must not touch the heap. An environment larger than the buffer is kept
// up to the last entry that fits.
constexpr size_t ENV_BUF_SIZE = 64 * 1024;
constexpr size_t ENV_MAX_ENTRIES = 2048;
char g_env_buf[ENV_BUF_SIZE];
const char *g_env_entries[ENV_MAX_ENTRIES];
size_t g_env_count = 0;
bool g_environment_captured = false;
}

void linkpy_capture_environment(char **envp)
{
    size_t used = 0;
    g_env_count = 0;
    for (char **e = envp; e != nullptr && *e != nullptr; e++)
    {
        const size_t len = std::strlen(*e) + 1;
        if (used + len > ENV_BUF_SIZE || g_env_count == ENV_MAX_ENTRIES)
            break;
        std::memcpy(g_env_buf + used, *e, len);
        g_env_entries[g_env_count++] = g_env_buf + used;
        used += len;
    }
    g_environment_captured = true;
}

const char *linkpy_getenv(const char *name)
{
    if (!g_environment_captured)
        return getenv(name);
    const size_t n = std::strlen(name);
    for (size_t i = 0; i < g_env_count; i++)
    {
        const char *entry = g_env_entries[i];
        if (std::strncmp(entry, name, n) == 0 && entry[n] == '=')
            return entry + n + 1;
    }
    return nullptr;
}

std::vector<std::string> linkpy_child_environment(const std::vector<std::string> &without)
{
    std::vector<std::string> env;
    if (!g_environment_captured)
    {
        for (char **e = environ; e != nullptr && *e != nullptr; e++)
            env.emplace_back(*e);
    }
    else
    {
        for (size_t i = 0; i < g_env_count; i++)
            env.emplace_back(g_env_entries[i]);
    }
    std::vector<std::string> kept;
    for (const auto &entry : env)
    {
        bool drop = false;
        for (const auto &name : without)
            if (entry.compare(0, name.size() + 1, name + "=") == 0)
                drop = true;
        if (!drop)
            kept.push_back(entry);
    }
    return kept;
}

std::string linkstub_cache_dir()
{
    const char *dir = linkpy_getenv("LINKPY_CACHE_DIR");
    if (dir != nullptr)
        return dir; // "" disables caching
    const char *xdg = linkpy_getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && *xdg != '\0')
        return std::string(xdg) + "/linkpy";
    const char *home = linkpy_getenv("HOME");
    if (home != nullptr && *home != '\0')
        return std::string(home) + "/.cache/linkpy";
    return "";
}

std::string linkstub_cache_read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string linkstub_cache_build_id(const void *addr)
{
    Dl_info info;
    if (dladdr(addr, &info) == 0 || info.dli_fbase == nullptr)
        return "";
    const auto *base = static_cast<const unsigned char *>(info.dli_fbase);
    const auto *ehdr = reinterpret_cast<const ElfW(Ehdr) *>(base);
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
        return "";
    const auto *phdr = reinterpret_cast<const ElfW(Phdr) *>(base + ehdr->e_phoff);
    for (unsigned i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr[i].p_type != PT_NOTE)
            continue;
        // A shared object's p_vaddr is relative to its load base.
        const unsigned char *p = base + phdr[i].p_vaddr;
        const unsigned char *end = p + phdr[i].p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end)
        {
            const auto *note = reinterpret_cast<const ElfW(Nhdr) *>(p);
            const unsigned char *name = p + sizeof(ElfW(Nhdr));
            const unsigned char *desc = name + ((note->n_namesz + 3) & ~3u);
            if (desc + note->n_descsz > end)
                break;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4
                && std::memcmp(name, "GNU", 4) == 0)
            {
                std::string hex;
                char buf[3];
                for (unsigned j = 0; j < note->n_descsz; j++)
                {
                    std::snprintf(buf, sizeof buf, "%02x", desc[j]);
                    hex += buf;
                }
                return hex;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return "";
}

std::string linkstub_cache_key(const std::vector<std::string> &material)
{
    // Each item is either a literal ("text") or a file to hash by content
    // ("@/path"); files stream through a fixed buffer so that even a large
    // auditor binary never becomes a heap allocation of its own size.
    Sha256 sha;
    static unsigned char buf[64 * 1024];
    for (const auto &item : material)
    {
        if (!item.empty() && item[0] == '@')
        {
            std::ifstream in(item.c_str() + 1, std::ios::binary);
            while (in)
            {
                in.read(reinterpret_cast<char *>(buf), sizeof buf);
                sha.update(buf, static_cast<size_t>(in.gcount()));
            }
        }
        else
        {
            sha.update(item.data(), item.size());
        }
        sha.update("", 1);
    }
    return sha.hex();
}

std::string linkstub_cache_lookup(const std::string &key)
{
    const std::string dir = linkstub_cache_dir();
    if (dir.empty())
        return "";
    const fs::path so = fs::path(dir) / key / "liblinkstub.so";
    std::error_code ec;
    return fs::is_regular_file(so, ec) ? so.string() : "";
}

std::string linkstub_cache_store(const std::string &key, const std::string &built_so,
                                 const std::string &source_cpp)
{
    const std::string dir = linkstub_cache_dir();
    if (dir.empty())
        return "";
    std::error_code ec;
    const fs::path entry = fs::path(dir) / key;
    fs::create_directories(entry, ec);
    if (ec)
        return "";
    // Copy next to the final name, then rename: a concurrent reader either
    // sees no file or the whole one.
    const fs::path tmp = entry / ("liblinkstub.so." + std::to_string(getpid()));
    fs::copy_file(built_so, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return "";
    const fs::path final_so = entry / "liblinkstub.so";
    fs::rename(tmp, final_so, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return "";
    }
    fs::copy_file(source_cpp, entry / "linkstub.cpp", fs::copy_options::overwrite_existing, ec);
    return final_so.string();
}
