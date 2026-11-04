// The marshaller driven directly by a C++ host that catches its exceptions and
// keeps calling, the way a server written in C++ would: a call that fails
// (the Python function raises; an argument cannot be converted) must leave
// the module's and the functions' reference counts as they were. The counts
// are read from Python (sys.getrefcount) through a third function.
#include <pycpputils.hpp>
#include <cstdio>
#include <stdexcept>

static long refs()
{
    static linkpy::py::CallSite site{"mod", "refs"};
    return linkpy::py::run_function<long>(site, 0);
}

int main()
{
    static linkpy::py::CallSite boom{"mod", "boom"};
    static linkpy::py::CallSite echo{"mod", "echo"};
    const int rounds = 50;
    int caught = 0;
    const char *bad = "\xff";   // a variable: a literal would deduce as an array and cross as bytes

    // One call on each path first, so the one-off costs (import, interning)
    // are not in the comparison.
    try { linkpy::py::run_function<int>(boom, 0); } catch (const std::runtime_error &) {}
    try { linkpy::py::run_function_void(echo, 1, bad); } catch (const std::runtime_error &) {}
    const long before = refs();

    for (int i = 0; i < rounds; i++)
    {
        // The function raises: run_function prints the traceback and throws.
        try { linkpy::py::run_function<int>(boom, 0); } catch (const std::runtime_error &) { caught++; }
        // The argument is not valid UTF-8: the conversion throws before the call.
        try { linkpy::py::run_function_void(echo, 1, bad); } catch (const std::runtime_error &) { caught++; }
    }
    const long after = refs();
    std::printf("marshaller_refs: caught %d of %d, refcounts before %ld after %ld\n",
                caught, 2 * rounds, before, after);
    return (caught == 2 * rounds && after == before) ? 0 : 1;
}
