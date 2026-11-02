// Phase 5: JS functions as C function pointers (mirror of py/cclosures).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const closures = allocs.load(fixtures + "/closures/closures.so");

function hi() { console.log("Hi"); }
function addOne(i) { return i + 1; }

closures.repeat(3, allocs.void.fun()(hi));

const addOneClosure = allocs.int.fun(allocs.int)(addOne);
console.log(closures.fold_int(5, addOneClosure));
console.log(closures.fold_int(42, addOneClosure));

console.log(closures.fold_int(10, allocs.int.fun(allocs.int)((x) => x * 2 + 1)));
