// Phase 4: proxies and their backing chunks are reclaimed when unreferenced;
// bulk churn must not accumulate (mirror of lua/gc_simple, scaled down).
// Verify with the liballocs exit summary on stderr if in doubt.
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const c = allocs.load(fixtures + "/composite/composite.so");

(async () => {
    for (let i = 1; i <= 100000; i++) {
        const hw = c.hello_world(i % 100, 0.5);
    }
    gc();
    await new Promise(setImmediate);
    gc();
    await new Promise(setImmediate);
    console.log("done");

    // a released proxy is inert
    const hw = c.hello_world(1, 2.0);
    hw.release();
    console.log(String(hw));
    try {
        void hw.hello;
        console.log("unexpected: no throw");
    } catch (e) {
        console.log(false, /released foreign proxy/.test(e.message));
    }
})();
