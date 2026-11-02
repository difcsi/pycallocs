#include <luacpputils.hpp>

// The Lua sibling of template.cpp: the auditor prepends this file to the
// generated linkstub.cpp, so it brings luacpputils.hpp into scope for the
// trampolines and manages the linkstub's Lua VM. A constructor creates the
// shared lua_State when liblinkstub loads; a destructor closes it on unload.


#ifdef DEBUG
#include <iostream>
#endif

__attribute__((constructor)) void init_vms()
{
    linkpy::lua::init_lua();

#ifdef DEBUG
    std::cout << "LuaVM Initialized from linkstub" << std::endl;
#endif
}

__attribute__((destructor)) void cleanup_vms()
{
    linkpy::lua::close_lua();
#ifdef DEBUG
    std::cout << "LuaVM Cleaned Up from linkstub" << std::endl;
#endif
}
