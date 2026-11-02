// Phase 1: load fixture libraries, read scalar and enum globals, inspect
// published foreign types (mirror of tests/lua/basic_load).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

// scalar globals
const alpha = allocs.load(fixtures + "/alphanum/alphanum.so");
console.log(alpha.NB_LETTERS, alpha.NB_DIGITS);

// enum global: reads as its integer value
const enums = allocs.load(fixtures + "/enums/enums.so");
console.log(enums.favourite);

// module-level base types are callable foreign types
console.log(String(allocs.int));
console.log(String(allocs.double));
console.log(String(allocs.char));

// unsupported exports are reported, not fatal
console.log("has __unsupported:", typeof alpha.__unsupported === "object");
