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

void print_hw(struct hello_world hw)
{
    printf("Hello: %d, World: %.2f\n", hw.hello, hw.world);
}

void compl_hw(struct hello_world *hw)
{
    hw->world = hw->hello;
}

static struct hello_world *GLOBAL_HW;
void save_hw(struct hello_world *hw)
{
    GLOBAL_HW = hw;
}
struct hello_world *retrieve_hw()
{
    return GLOBAL_HW;
}
struct hello_world *swap_saved_hw(struct hello_world *hw)
{
    struct hello_world *ret_hw = GLOBAL_HW;
    GLOBAL_HW = hw;
    return ret_hw;
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

void print_cat(union quantum_cat qc)
{
    printf("Dead (%d) or alive (%.2f)\n", qc.dead, qc.alive);
}
