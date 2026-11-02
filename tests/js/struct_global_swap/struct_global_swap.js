// Phase 4: the defer-free window. swap_saved_hw overwrites the C global that
// holds the only reference to the 42-object and returns that object; without
// Begin/EndDeferFrees around ffi_call the delref would free it mid-call
// (mirror of lua/struct_global_swap).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const c = allocs.load(fixtures + "/composite/composite.so");

(async () => {
    {
        let a = c.hello_world(42, 9.99);
        c.save_hw(a);
        a = null;
    }
    gc();
    await new Promise(setImmediate);
    gc();
    await new Promise(setImmediate);

    const b = c.hello_world(0, 0.0);

    console.log(String(c.swap_saved_hw(b)));
    console.log(String(c.swap_saved_hw(null)));
})();
