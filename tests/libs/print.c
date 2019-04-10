#include <stdio.h>
#include <stdbool.h>

void hello()
{
    printf("Hello world!\n");
}

void print_int(int i)
{
    printf("%d\n", i);
}

void print_bool(bool b)
{
    printf("%d\n", b);
}

void print_float(float f)
{
    printf("%g\n", f);
}

void print_char(char c)
{
    printf("%c\n", c);
}

void print_str(const char* str)
{
    printf("%s\n", str);
}

void print_multiargs(long i1, long double f, short i2)
{
    printf("%ld %Lg 0x%hx\n", i1, f, i2);
}
