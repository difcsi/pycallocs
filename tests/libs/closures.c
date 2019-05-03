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
