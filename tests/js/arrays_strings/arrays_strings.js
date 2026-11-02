// Phase 2: array/pointer proxies over fixture globals -- length, 0-based
// element access, Array.from, writes, slices (end-exclusive), char arrays as
// strings (mirror of tests/lua/arrays_strings with JS indexing conventions).
const allocs = require(process.env.V8PROXY_MODULE);
const fixtures = process.env.PYCALLOCS_TEST_LIBS;

const a = allocs.load(fixtures + "/alphanum/alphanum.so");

// char array global (static length includes the NUL element)
console.log(a.ALPHABET.length);
console.log(a.ALPHABET.str());
console.log(String(a.ALPHABET));
console.log(a.ALPHABET[0], a.ALPHABET[25]);

// int array global
console.log(a.DIGITS.length);
console.log(a.DIGITS[0], a.DIGITS[9]);
console.log(String(a.DIGITS));

// out-of-range reads give undefined; Array.from works via length+indices
console.log(a.DIGITS[10]);
const sum = Array.from(a.DIGITS).reduce((x, y) => x + y, 0);
console.log(sum);

// element writes hit the real memory
a.DIGITS[0] = 42;
console.log(a.DIGITS[0]);
a.DIGITS[0] = 0;

// slices are interior proxies anchored to their source (0-based, end-exclusive)
const mid = a.DIGITS.slice(2, 5);
console.log(mid.length, mid[0], mid[2]);
mid[0] = 20;
console.log(a.DIGITS[2]);
mid[0] = 2;
