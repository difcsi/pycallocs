void repeat(int times, void (*fun)(void))
{
    while (times--) fun();
}

int fold_int(int nb, int (*fun)(int))
{
    int acc = 0;
    for (int i = 0; i < nb; i++)
    {
        acc = fun(acc);
    }
    return acc;
}

struct closure_struct
{
    void (*fun1)(void);
    void (*fun2)(void);
    void (*fun3)(void);
};

void call_closure_struct(struct closure_struct* cs)
{
    cs->fun1();
    cs->fun2();
    cs->fun3();
}
