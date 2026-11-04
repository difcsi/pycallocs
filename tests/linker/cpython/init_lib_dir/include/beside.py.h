/* A module that lives next to the init library (where an installed language
 * module keeps its Python half) is importable without any PYTHONPATH: the
 * stub library's directory is on the module search path, like `src`. */
extern int value_beside();
