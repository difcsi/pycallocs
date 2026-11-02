#include <stdio.h>
#include <string.h>
#include "blob.py.h"

int main(void)
{
    struct blob b = { .data = "ab\0cd", .len = 5 };
    struct blob r = echo(b);
    int echo_ok = r.len == 5 && memcmp(r.data, "dc\0ba", 5) == 0
                  && r.data[5] == 0 && r.data[15] == 0;

    struct tag t = label(7);
    int label_ok = strcmp(t.name, "tag7") == 0 && t.n == 7;

    printf("echo ok=%d label ok=%d\n", echo_ok, label_ok);
    return (echo_ok && label_ok) ? 0 : 1;
}
