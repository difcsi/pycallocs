/* Fixture for the heap-hints tests. Exercises every case of the escape rule:
 * a function is listed iff it has a pointer argument that is NOT a single-level
 * char/const char pointer (those become a by-value Python str and don't escape).
 *
 * Expected listed (sorted): argv_dump, mirror, sum.
 */

struct Box {
    int v;
};

extern void mirror(struct Box *b);     /* non-char pointer -> listed */
extern int  sum(int *xs, int n);       /* int*             -> listed */
extern int  scale(int factor);         /* scalar           -> not listed */
extern void greet(const char *name);   /* const char*      -> not listed (str) */
extern int  describe(struct Box b);    /* by value         -> not listed */
extern void argv_dump(char **argv);    /* char**           -> listed */
