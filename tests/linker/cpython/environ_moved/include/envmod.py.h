/* A host that rewrites its environment block after start-up (servers do it
 * for their process title) must still be able to load the linkstub: the
 * auditor reads its settings when the process starts, not at load time. */
extern int answer();
