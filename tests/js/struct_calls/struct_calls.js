// Phase 3: structs and unions across call boundaries -- by value, by
// pointer, out-parameter cells, and the C-keeps-the-pointer round-trip
// (mirror of tests/lua/struct_calls).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const c = allocs.load(fixtures + "/composite/composite.so");

// struct return by value
const hw = c.make_hw(4, 8.5);
console.log(hw.hello, hw.world);

// struct passed by value (C printf output)
c.print_hw(hw);

// struct passed by pointer: callee mutates through the pointer
c.compl_hw(hw);
console.log(hw.world);

// read-only by-pointer accessor
console.log(c.sum_hw(hw));

// a plain object literal is coerced by value where a struct is expected
c.print_hw({ hello: 1, world: 0.25 });

// unions across the boundary
const qc = c.make_dead(7);
console.log(qc.dead);

// scalar out-parameter cell: C sees int*, JS reads the result back
const w = allocs.int(5);
console.log(w.length, w[0]);

// pointer round-trip: C keeps the pointer, hands the same object back
c.save_hw(hw);
const back = c.retrieve_hw();
console.log(back.hello);
back.hello = 11;
console.log(hw.hello);
