/* Spelled-out base-type uniqtype names that the pycallocs extension imports,
 * aliased to the canonical names liballocs' dwarftypes emits.
 *
 * This file is appended (by gen_provider.sh) to the generated dwarftypes
 * translation unit, so that each `.set` alias's target symbol is defined in the
 * same object -- a `.set` to a symbol defined in another TU is silently dropped.
 *
 *   spelled-out (what allocs.so imports)   ->   canonical (what dwarftypes emits)
 */
__asm__(
".globl __uniqtype__short_int$$16\n"           ".set __uniqtype__short_int$$16, __uniqtype__int$$16\n"
".globl __uniqtype__short_unsigned_int$$16\n"  ".set __uniqtype__short_unsigned_int$$16, __uniqtype__uint$$16\n"
".globl __uniqtype__unsigned_int$$32\n"        ".set __uniqtype__unsigned_int$$32, __uniqtype__uint$$32\n"
".globl __uniqtype__long_int$$64\n"            ".set __uniqtype__long_int$$64, __uniqtype__int$$64\n"
".globl __uniqtype__long_unsigned_int$$64\n"   ".set __uniqtype__long_unsigned_int$$64, __uniqtype__uint$$64\n"
".globl __uniqtype__float32\n"                 ".set __uniqtype__float32, __uniqtype__float$$32\n"
".globl __uniqtype__double64\n"                ".set __uniqtype__double64, __uniqtype__float$$64\n"
);
