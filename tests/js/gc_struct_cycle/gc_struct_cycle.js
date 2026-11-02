// Phase 5 demo, NOT a parity test: host-C reference cycles are uncollectable
// in the V8 port (like the Lua one). V8's GC has no traverse protocol over
// foreign memory, so a C struct whose pointer field keeps its own proxy alive
// (via the strong slot->proxy map) can never be reclaimed automatically.
// The documented policy: such cycles leak unless broken with release().
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const m = allocs.load(fixtures + "/bigrecursive/bigrecursive.so");

(async () => {
    // this one leaks (by design): the C self-pointer pins the proxy forever
    {
        let br = m.bigrecursive();
        br.ptr = br;
        br = null;
    }
    gc();
    await new Promise(setImmediate);
    console.log("cycle dropped (leaks by design)");

    // release() breaks the cycle by hand: slots cleared, chunk freed
    {
        let br2 = m.bigrecursive();
        br2.ptr = br2;
        br2.release();
        br2 = null;
    }
    gc();
    await new Promise(setImmediate);
    console.log("released cycle reclaimed");
})();
