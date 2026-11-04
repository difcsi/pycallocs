/* A by-value struct with padding between its members (7 bytes after c, 7
 * after d). The padding of a result must be zero in the C caller: a struct
 * is compared, hashed or written out whole. */
struct padded {
    char c;
    long x;
    char d;
    long y;
};

extern struct padded make_padded(void);
extern long sum_padded(struct padded p);
