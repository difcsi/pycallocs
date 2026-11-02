// Phase 2: struct/union construction and manipulation from JS (mirror of
// tests/lua/struct_manip; object literals replace Lua's named tables).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const c = allocs.load(fixtures + "/composite/composite.so");

// published types are callable foreign types
console.log(String(c.hello_world));

// positional construction
const hw = c.hello_world(5, 2.5);
console.log(String(hw));
console.log(hw.hello, hw.world);

// field writes
hw.hello = 7;
hw.world = 0.5;
console.log(hw.hello, hw.world);

// object-literal construction; absent fields are zero-initialised
const hw2 = c.hello_world({ world: 1.5 });
console.log(hw2.hello, hw2.world);

// copy construction: same values, distinct object
const hw3 = c.hello_world(hw);
console.log(hw3.hello, hw3.world);
hw3.hello = 100;
console.log(hw.hello, hw3.hello);

// zero-initialising constructor
const z = c.hello_world();
console.log(z.hello, z.world);

// Object.keys sees the C fields (enumerator callback)
console.log(Object.keys(hw).join(","));

// unions: fields alias the same storage
const qc = c.quantum_cat();
qc.dead = 1;
console.log(qc.dead);
qc.alive = 0.25;
console.log(qc.alive);
