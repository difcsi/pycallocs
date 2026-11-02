// Phase 3: calling foreign functions through libffi (mirror of py/basic_ops).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const m = allocs.load(fixtures + "/basic/basic.so");

console.log(m.triple(3));
console.log(m.inv(true));
console.log(m.mul(1.02, 5));
console.log(m.a());
console.log(String(m.triple));
