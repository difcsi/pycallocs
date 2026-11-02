// Phase 4: a C global pointer keeps a JS-constructed struct alive after the
// last JS reference is dropped and collected (mirror of lua/struct_global).
// The trapptrwrites barrier in the fixture drives the liballocs GC-policy
// callbacks maintaining the strong slot->proxy map. Runs under --expose-gc;
// weak second-pass callbacks need a macrotask turn to run, hence the
// gc-then-setImmediate idiom.
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const c = allocs.load(fixtures + "/composite/composite.so");

async function drain() {
    gc();
    await new Promise(setImmediate);
    gc();
    await new Promise(setImmediate);
}

(async () => {
    console.log(String(c.retrieve_hw()));

    {
        let a = c.hello_world();
        a.hello = 42;
        a.world = 9.99;
        c.save_hw(a);
        a = null;
    }
    await drain();
    console.log(String(c.retrieve_hw()));

    {
        let b = c.hello_world();
        b.hello = 0;
        b.world = 0.0;
        c.save_hw(b);
        b = null;
    }
    await drain();
    console.log(String(c.retrieve_hw()));
})();
