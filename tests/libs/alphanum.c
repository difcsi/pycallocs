int NB_LETTERS = 26;
int NB_DIGITS = 10;

char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz";
int DIGITS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

char *get_alphabet()
{
    return &ALPHABET;
}

int *get_digits()
{
    return &DIGITS;
}
