/* A Python thread started inside one call must keep running while C is
 * busy between calls: the linkstub releases the GIL whenever no trampoline
 * is executing Python. */
extern void start();
extern int count();
