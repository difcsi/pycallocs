/* `src` is added to the module search path only when it exists next to the
 * working directory at start-up, and then as an absolute path: a server's
 * cwd is wherever its application lives. */
extern int relative_src_on_path();
