/* Forked-VM milestone: several lua_States in one process share the
 * liballocs integration -- each has its own foreign values, chunk
 * lifetime is shared through process-wide refcounts, and one state's
 * close never frees memory another state still views.
 *
 * Built and run by tests/vmextensions/lua/run_tests.sh in --native
 * mode, linked against the fork's liblua.a. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static void run (lua_State *L, const char *code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
    exit(1);
  }
}

int main (void) {
  lua_State *L1, *L2;
  char buf[256];
  lua_Integer addr;

  L1 = luaL_newstate();
  luaL_openlibs(L1);
  run(L1, "local c = allocs.load(os.getenv('PYCALLOCS_TEST_LIBS')"
          " .. '/composite/composite.so')\n"
          "hw = c.make_hw(7, 1.5)\n"
          "addr = allocs.address_of(hw)\n"
          "print('L1 sees:', hw.hello)");

  lua_getglobal(L1, "addr");
  addr = lua_tointeger(L1, -1);
  lua_pop(L1, 1);

  L2 = luaL_newstate();
  luaL_openlibs(L2);
  snprintf(buf, sizeof buf,
           "hw2 = allocs.from_address(%lld)\n"
           "print('L2 sees:', hw2.hello)\n"
           "hw2.hello = 42", (long long)addr);
  run(L2, buf);

  /* the write through L2 lands in the shared C memory */
  run(L1, "print('L1 sees after L2 write:', hw.hello)");

  /* closing L1 must not free the chunk: L2 still holds a reference */
  lua_close(L1);
  run(L2, "collectgarbage(); collectgarbage()\n"
          "print('L2 sees after L1 close:', hw2.hello)");
  lua_close(L2);

  printf("multistate done\n");
  return 0;
}
