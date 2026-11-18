#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include "ccord_test.h"

int main(void)
{
  CT_CHECK(strcmp(LUA_RELEASE, "Lua 5.5.1") == 0); // LUA_VERSION_RELEASE is only the patch digit "1"
  CT_CHECK(LUA_VERSION_NUM == 505);

  lua_State *L = luaL_newstate();
  CT_CHECK(L != nullptr);
  if (L) {
    CT_CHECK(luaL_dostring(L, "return 1 + 1") == LUA_OK);
    CT_CHECK(lua_tointeger(L, -1) == 2);
    lua_close(L);
  }
  return CT_RESULT("test_lua_version");
}
