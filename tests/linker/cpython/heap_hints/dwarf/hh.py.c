/* Fixture for the DWARF-based heap-hints test (the standalone `heaphints` tool).
 * Defined (not just declared) so the functions carry DW_AT_low_pc, and named
 * *.py.c so load_dwarf_map's stub decl-file filter accepts them. Same cases as
 * include/hh.py.h; expected listed (sorted): argv_dump, mirror, sum. */

struct Box {
    int v;
};

void mirror(struct Box *b) { (void)b; }
int  sum(int *xs, int n) { (void)xs; return n; }
int  scale(int factor) { return factor; }
void greet(const char *name) { (void)name; }
int  describe(struct Box b) { return b.v; }
void argv_dump(char **argv) { (void)argv; }
