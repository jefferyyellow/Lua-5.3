extern "C"
{
#include "lua.h"  
#include "lauxlib.h"  
#include "lualib.h"  
}
#include "../lua_tinker/lua_tinker.h"

//#define MAXBITS		26
//#define MAXASIZE	(1 << MAXBITS)
//
//int average(lua_State* L)
//{
//	int lua_args_count = lua_gettop(L);
//	lua_Number sum = 0;
//	for (int i = 1; i <= lua_args_count; ++ i)
//	{
//		sum += lua_tonumber(L, i);
//	}
//	lua_pushnumber(L, sum/lua_args_count);
//	lua_pushnumber(L, sum);
//	return 2;
//}
//
//int main()
//{
//	char buff[256];
//	int error;
//	lua_State *L = luaL_newstate();
//	luaL_openlibs(L);
//
//	// -----------------全局变量-----------------
//	// 实则全局变量的值
//	lua_pushstring(L, "++++++test new value +++++++");
//	lua_setglobal(L, "global_c_write_value");
//
//	luaL_dofile(L, "luatest.lua");
//	// 得到全局变量的值
//	lua_getglobal(L, "global_c_read_val");
//	if (const char* val = lua_tostring(L, -1))
//	{
//		printf("%s\n", val);
//	}
//	printf("----------------------------------------------------------\n");
//	// -----------------全局变量-----------------
//
//	// -----------------全局Table-----------------
//	lua_newtable(L);
//	lua_pushstring(L, "integer_val");
//	lua_pushinteger(L, 1234);
//	lua_settable(L, -3);
//	lua_setglobal(L, "global_c_write_table");
//
//	luaL_dofile(L, "lua_global_table.lua");
//	
//	lua_getglobal(L, "global_c_read_table");
//	lua_pushstring(L, "integer_val");
//	lua_gettable(L, -2);
//	if (lua_isinteger(L, -1))
//	{
//		printf("integar_val: %lld\n", lua_tointeger(L, -1));
//	}
//	lua_pop(L, 1);
//
//	lua_pushstring(L, "double_val");
//	lua_gettable(L, -2);
//	if (lua_isnumber(L, -1))
//	{
//		printf("double_val: %f\n", lua_tonumber(L, -1));
//	}
//	lua_pop(L, 1);
//
//	//lua_pushstring(L, "string_val");
//	//lua_gettable(L, -2);
//	lua_getfield(L, -1, "string_val");
//	if (lua_isstring(L, -1))
//	{
//		printf("string_val: %s\n", lua_tostring(L, -1));
//	}
//	lua_pop(L, 1);
//	// -----------------全局Table-----------------
//	printf("----------------------------------------------------------\n");
//	// -----------------全局Array-----------------
//	lua_newtable(L);
//	lua_pushinteger(L, 7);
//	lua_rawseti(L, -2, 1);
//
//	lua_pushnumber(L, 8.9);
//	lua_rawseti(L, -2, 2);
//
//	lua_pushstring(L, "test_string_*:*:*:");
//	lua_rawseti(L, -2, 3);
//	lua_setglobal(L, "global_c_write_array");
//
//	luaL_dofile(L, "lua_global_array.lua");
//
//
//	lua_getglobal(L, "global_c_read_array");
//	lua_Integer arrayLen = luaL_len(L, -1);
//	for (lua_Integer i = 1; i <= arrayLen; ++ i)
//	{
//		int ret_type = lua_rawgeti(L, -1, i);
//		switch (ret_type)
//		{
//			case LUA_TNUMBER:
//			{
//				if (lua_isinteger(L, -1))
//				{
//					printf("%lld\n", lua_tointeger(L, -1));
//				}
//				else if (lua_isnumber(L, -1))
//				{
//					printf("%f\n", lua_tonumber(L, -1));
//				}
//				break;
//			}
//			case LUA_TSTRING:
//			{
//				printf("%s\n", lua_tostring(L, -1));
//				break;
//			}
//		}
//		lua_pop(L, 1);
//	}
//	// -----------------全局Array-----------------
//	printf("----------------------------------------------------------\n");
//
//	// -----------------全局函数-----------------
//	lua_register(L, "average", average);
//
//	luaL_dofile(L, "lua_global_function.lua");
//	lua_getglobal(L, "add");
//	lua_pushinteger(L, 123);
//	lua_pushinteger(L, 456);
//
//	lua_call(L, 2, 1);
//
//	printf("add function return value : %d\n", lua_tointeger(L, -1));
//	// -----------------全局函数-----------------
//	lua_close(L);
//}
//


// ------------------ lua_thinker test ------------------ 
//
//class CBase
//{
//public:
//	CBase(){}
//	~CBase(){}
//
//public:
//	const char* IsBase(){ return "this is Base";}
//	int GetBaseValue() const { return mBaseValue; }
//	void SetBaseValue(int nValue) { mBaseValue = nValue; }
//	int mBaseValue;
//};
//
//class CObject
//{
//public:
//	CObject()
//	{
//		mValue = 0;
//	}
//	~CObject(){}
//
//public:
//	int GetValue() const { return mValue; }
//	void SetValue(int nValue) { mValue = nValue; }
//
//private:
//	int mValue;
//};
//class CTest : public CBase
//{
//public:
//	typedef CObject& (CTest::*GetObjectType)();
//
//public:
//	CTest(int nVal) : mValue(nVal){}
//	~CTest(){}
//
//public:
//	const char* IsTest(){ return "this is Test"; }
//	void RetVoid(){}
//	int RetInt(){ return mValue; }
//	int RetMul(int m){return mValue * m;}
//	int GetValue(){return mValue;}
//	void SetValue(int nValue){mValue = nValue;}
//	CObject& GetObject(){return mObject;}
//	int* GetArray()
//	{
//		//printf("%p", mArray);
//		return mArray;
//	}
//	int GetArrayByIndex(int nIndex)
//	{
//		return mArray[nIndex];
//	}
//
//	int mValue;
//	int mArray[100];
//	CObject		mObject;
//};
//
//void paramObject(CTest& rTest)
//{
//	//printf("%d\n", rTest.GetValue());
//	rTest.SetValue(820);
//}
//
//void changeValue(int* pValue)
//{
//	pValue[0] = 1024;
//	pValue[1] = 10256;
//	pValue[2] = 23434;
//}
//
//int main()
//{
//	lua_State *L = luaL_newstate();
//	if (NULL == L)
//	{
//		printf("bad luaL_newstate");
//		return -1;
//	}
//	luaL_openlibs(L);
//
//	lua_tinker::class_add<CBase>(L, "CBase");
//	lua_tinker::class_con<CBase>(L, lua_tinker::constructor<CBase>);
//	lua_tinker::class_mem<CBase>(L, "mBaseValue", &CBase::mBaseValue);
//	lua_tinker::class_def<CBase>(L, "IsBase", &CBase::IsBase);
//	lua_tinker::class_def<CBase>(L, "GetBaseValue", &CBase::GetBaseValue);
//	lua_tinker::class_def<CBase>(L, "SetBaseValue", &CBase::SetBaseValue);
//
//	lua_tinker::class_add<CTest>(L, "CTest");
//	lua_tinker::class_inh<CTest, CBase>(L);
//	lua_tinker::class_con<CTest>(L, lua_tinker::constructor<CTest, int>);
//	
//	lua_tinker::class_mem<CTest>(L, "mValue", &CTest::mValue);
//	lua_tinker::class_def<CTest>(L, "IsTest", &CTest::IsTest);
//	lua_tinker::class_def<CTest>(L, "RetInt", &CTest::RetInt);
//	lua_tinker::class_def<CTest>(L, "RetMul", &CTest::RetMul);
//	lua_tinker::class_def<CTest>(L, "GetValue", &CTest::GetValue);
//	lua_tinker::class_def<CTest>(L, "SetValue", &CTest::SetValue);
//	lua_tinker::class_def<CTest>(L, "GetObject", &CTest::GetObject);
//	lua_tinker::class_def<CTest>(L, "GetArray", &CTest::GetArray);
//	lua_tinker::class_def<CTest>(L, "GetArrayByIndex", &CTest::GetArrayByIndex);
//
//
//	lua_tinker::class_add<CObject>(L, "CObject");
//	lua_tinker::class_def<CObject>(L, "SetValue", &CObject::SetValue);
//	lua_tinker::class_def<CObject>(L, "GetValue", &CObject::GetValue);
//
//
//	lua_tinker::def(L, "paramObject", &paramObject);
//	lua_tinker::def(L, "changeValue", &changeValue);
//
//	lua_tinker::dofile(L, "lua_tinker.lua");
//	lua_close(L);
//}
//
// ------------------ lua_thinker test ------------------ 


int main()
{
	char buff[256];
	int error;
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	luaL_dofile(L, "luatest.lua");
}