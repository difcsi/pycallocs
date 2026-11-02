/* The host configures the interpreter's environment (PYTHONPATH here) only
 * after loading the init library, as servers do when they set up an
 * application after loading its language module: the interpreter must not
 * start before the first call. */
extern int answer_late();
