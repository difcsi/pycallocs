/* Fixed-size char arrays inside by-value structs cross as Python bytes:
 * C -> Python the whole array (b.data is 16 bytes long, b.len says how much
 * of it means anything), Python -> C a bytes of at most the array length,
 * zero-filled to the end (so t.name is NUL-terminated for C). */
struct blob {
    unsigned char data[16];
    int len;
};

struct tag {
    char name[8];
    int n;
};

extern struct blob echo(struct blob b);
extern struct tag label(int n);
