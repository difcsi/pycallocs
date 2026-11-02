// Phase 5: a C struct field is the only thing keeping a closure alive
// (mirror of py/closure_lifetime). The trampoline lives in mmap'd memory
// liballocs cannot track, so retention comes from Proxy_RetainStoredPtr.
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const closures = allocs.load(fixtures + "/closures/closures.so");

(async () => {
    let cat = () => console.log("Meow.");
    let dog = () => console.log("Woof!");

    const cs = closures.closure_struct();
    cs.fun1 = allocs.void.fun()(cat);
    cs.fun2 = allocs.void.fun()(dog);
    cs.fun3 = allocs.void.fun()(cat);
    cat = null;
    dog = null;
    gc();
    await new Promise(setImmediate);
    gc();
    await new Promise(setImmediate);

    closures.call_closure_struct(cs);
})();
