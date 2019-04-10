#include <stdio.h>

struct hello_world
{
    int hello;
    double world;
};

struct hello_world make_hw(int h, float w)
{
    struct hello_world hw = { h, w };
    return hw;
}

void print_hw(struct hello_world *hw)
{
    printf("Hello: %d, World: %f", hw->hello, hw->world);
}

void compl_hw(struct hello_world *hw)
{
    hw->world = hw->hello;
}

union quantum_cat
{
    int dead;
    double alive;
};

union quantum_cat make_dead(int d)
{
    union quantum_cat qc = { dead: d };
    return qc;
}

union quantum_cat make_alive(double f)
{
    union quantum_cat qc = { alive: f };
    return qc;
}
