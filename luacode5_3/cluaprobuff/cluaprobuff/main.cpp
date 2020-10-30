#include "loginmessage.hxx.pb.h"
extern "C"
{
#include "lua.h"  
#include "lauxlib.h"  
#include "lualib.h"  
}
#include "lua_tinker.h"

int main()
{
	CLoginRequest tRequest;
	tRequest.set_name("hjhasdf");
	tRequest.set_channelid(1032423);
	tRequest.set_serverid(213412);

	char acBuffer[1024] = {0};
	tRequest.SerializeToArray(acBuffer, 1024);

	lua_State *L = luaL_newstate();
	luaL_openlibs(L);
	lua_tinker::dofile(L, "test.lua");

	lua_tinker::call<int>(L, "printfMsg",acBuffer);

	lua_close(L);
	
}