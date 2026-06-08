/* Base-types uniqtype provider.
 *
 * The pycallocs extension references base-type uniqtype symbols (e.g.
 * __uniqtype__signed_char$$8) that, in a normal liballocs deployment, are
 * provided by per-binary meta-DSOs. We can't generate those from the C library
 * here (its DWARF-5 debug info crashes liballocs' dwarftypes), so instead we
 * compile this tiny translation unit -- which mentions every base type the
 * extension needs -- with DWARF-4 and run liballocs' meta generator over it.
 * The resulting meta-DSO defines the canonical uniqtypes; gen_provider.sh then
 * adds the spelled-out aliases the extension imports. See gen_provider.sh.
 */
signed char        bt_sc;
unsigned char      bt_uc;
short int          bt_s;
short unsigned int bt_us;
int                bt_i;
unsigned int       bt_ui;
long int           bt_l;
long unsigned int  bt_ul;
float              bt_f;
double             bt_d;
void bt_void_fn(void) {}   /* puts 'void' in the DWARF */
