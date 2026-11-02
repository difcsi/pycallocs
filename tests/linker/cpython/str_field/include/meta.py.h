/* char* fields inside a by-value struct cross C -> Python as str
 * (NULL -> None), exactly like char* arguments already do. */
struct meta {
    const char *name;
    char *note;
    int n;
};

extern int describe(struct meta m);
