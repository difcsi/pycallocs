#include <stdio.h>
#include <complex.h>

void print_complex(complex double z)
{
    printf("%f + i*%f\n", creal(z), cimag(z));
}
