#include <stdio.h>
#include "mytest.py.h"

int main(){
    struct HelloWorld hw = phelloworld2();
    printf("HelloWorld: %d\n", hw.a);
}