enum color { RED, GREEN = 5, BLUE };

// Takes and returns an enum: exercises enum handling through libffi calls.
enum color brighten(enum color c)
{
    return c + 1;
}

// Returns the integer value of an enum argument.
int as_int(enum color c)
{
    return (int) c;
}

// A global of enum type: exercises enum handling for STT_OBJECT symbols.
enum color favourite = GREEN;
