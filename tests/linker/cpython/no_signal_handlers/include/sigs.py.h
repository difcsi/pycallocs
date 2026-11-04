/* Starting the embedded interpreter must leave the host's signal
 * dispositions alone: a C program expects SIGINT to keep whatever it (or its
 * launcher) set, not Python's handler. Checked from C around the first call,
 * which is what starts the interpreter; the Python side must not import
 * `signal`, since that module installs the handler itself when SIGINT is at
 * its default (as it does for any embedding, Unit's included). */
extern int nothing();
