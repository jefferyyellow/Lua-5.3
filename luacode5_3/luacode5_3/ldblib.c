/*
** $Id: ldblib.c,v 1.151.1.1 2017/04/19 17:20:42 roberto Exp $
** Interface from Lua to its debug API
** See Copyright Notice in lua.h
*/

#define ldblib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


/*
** The hook table at registry[&HOOKKEY] maps threads to their current
** hook function. (We only need the unique address of 'HOOKKEY'.)
*/
// 注册表中的钩子表registry[&HOOKKEY]把线程映射到当前的钩子函数
// (我们只需要唯一的'HOOKKEY'的地址）
static const int HOOKKEY = 0;


/*
** If L1 != L, L1 can be in any state, and therefore there are no
** guarantees about its stack space; any push in L1 must be
** checked.
*/
// 如果L1 != L，那么L1可以处于任何状态，因此她的堆栈空间没用任何保证；
// L1上面的任何压栈都必须检查
// 如果L不等于L1，那么需要检查L1堆栈上是否能有n个空位
static void checkstack (lua_State *L, lua_State *L1, int n) {
  if (L != L1 && !lua_checkstack(L1, n))
    luaL_error(L, "stack overflow");
}

// 得到注册表
static int db_getregistry (lua_State *L) {
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  return 1;
}

// 得到元表
static int db_getmetatable (lua_State *L) {
  luaL_checkany(L, 1);
  // 得到元表，没有的话，得到nil
  if (!lua_getmetatable(L, 1)) {
    lua_pushnil(L);  /* no metatable */
  }
  return 1;
}

// 设置元表
static int db_setmetatable (lua_State *L) {
  // 得到第二个参数
  int t = lua_type(L, 2);
  // 是否是table或者nil
  luaL_argcheck(L, t == LUA_TNIL || t == LUA_TTABLE, 2,
                    "nil or table expected");
  // 必须有2个参数
  lua_settop(L, 2);
  // 设置元表
  lua_setmetatable(L, 1);
  return 1;  /* return 1st argument */
}

// 得到栈索引为1的，是否是USERDATA，是的话返回，不是返回nil
static int db_getuservalue (lua_State *L) {
    // 不是的话，将nil压栈
  if (lua_type(L, 1) != LUA_TUSERDATA)
    lua_pushnil(L);
  else
    lua_getuservalue(L, 1);
  return 1;
}

// 设置UserData
static int db_setuservalue (lua_State *L) {
  luaL_checktype(L, 1, LUA_TUSERDATA);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  lua_setuservalue(L, 1);
  return 1;
}


/*
** Auxiliary function used by several library functions: check for
** an optional thread as function's first argument and set 'arg' with
** 1 if this argument is present (so that functions can skip it to
** access their other arguments)
*/
// 几个库函数使用的辅助函数：检查可选线程作为函数的第一个参数，并使用如果存在此参数，则为1
// (以便函数能跳过该参数去访问其他的参数）
// 从第一个参数获得线程
static lua_State *getthread (lua_State *L, int *arg) {
  if (lua_isthread(L, 1)) {
    *arg = 1;
    return lua_tothread(L, 1);
  }
  else {
    *arg = 0;
    return L;  /* function will operate over current thread */
  }
}


/*
** Variations of 'lua_settable', used by 'db_getinfo' to put results
** from 'lua_getinfo' into result table. Key is always a string;
** value can be a string, an int, or a boolean.
*/
// lua_settable的变体，在函数db_getinfo中使用，用来将结果从lua_getinfo放入结果表中。
// 键总是字符串，值可以是字符串，整数或者bool值

static void settabss (lua_State *L, const char *k, const char *v) {
  // 值为字符串
  lua_pushstring(L, v);
  // 栈索引idx的为表，t[k] = value，value就是栈顶的值
  lua_setfield(L, -2, k);
}

static void settabsi (lua_State *L, const char *k, int v) {
  // 值为整数
  lua_pushinteger(L, v);
  // 栈索引idx的为表，t[k] = value，value就是栈顶的值
  lua_setfield(L, -2, k);
}

static void settabsb (lua_State *L, const char *k, int v) {
  // 值为bool
  lua_pushboolean(L, v);
  // 栈索引idx的为表，t[k] = value，value就是栈顶的值
  lua_setfield(L, -2, k);
}


/*
** In function 'db_getinfo', the call to 'lua_getinfo' may push
** results on the stack; later it creates the result table to put
** these objects. Function 'treatstackoption' puts the result from
** 'lua_getinfo' on top of the result table so that it can call
** 'lua_setfield'.
*/
// 在函数 'db_getinfo' 中，对 'lua_getinfo' 的调用可能把结果压入到堆栈上；
// 稍后它会创建结果表来放置这些对象。函数“ treatstackoption”将'lua_getinfo'得到的结果
// 放在结果表的顶部，以便它可以调用'lua_setfield'

static void treatstackoption (lua_State *L, lua_State *L1, const char *fname) {
  if (L == L1)
    // 将Object和Table调换一下
    lua_rotate(L, -2, 1);  /* exchange object and table */
  else
    // 将L1堆栈顶部的Object移动到L堆栈顶部
    lua_xmove(L1, L, 1);  /* move object to the "main" stack */
  // 将Object放在table中去
  lua_setfield(L, -2, fname);  /* put object into table */
}


/*
** Calls 'lua_getinfo' and collects all results in a new table.
** L1 needs stack space for an optional input (function) plus
** two optional outputs (function and line table) from function
** 'lua_getinfo'.
*/
// 调用 'lua_getinfo' 并将所有结果收集到一个新表中。L1 需要堆栈空间用于'lua_getinfo'的
// 可选输入（函数）加上函数的两个可选输出（函数和行表）
static int db_getinfo (lua_State *L) {
  lua_Debug ar;
  int arg;
  // 得到第一个参数对应线程信息，如果第一个参数不是线程，就用当前线程
  lua_State *L1 = getthread(L, &arg);
  // 得到参数中的选项
  const char *options = luaL_optstring(L, arg+2, "flnStu");
  checkstack(L, L1, 3);
  // 如果是一个函数（信息是关于一个函数的）
  if (lua_isfunction(L, arg + 1)) {  /* info about a function? */
    // 在原来的options前面增加'>'
    options = lua_pushfstring(L, ">%s", options);  /* add '>' to 'options' */
    // 将函数移到L1的堆栈上
    lua_pushvalue(L, arg + 1);  /* move function to 'L1' stack */
    lua_xmove(L, L1, 1);
  }
  // 还是指定栈帧的
  else {  /* stack level */
    // 找不到L1上的栈帧，栈帧已经超出范围了
    if (!lua_getstack(L1, (int)luaL_checkinteger(L, arg + 1), &ar)) {
      lua_pushnil(L);  /* level out of range */
      return 1;
    }
  }
  // lua_getinfo收集结果
  if (!lua_getinfo(L1, options, &ar))
    return luaL_argerror(L, arg+2, "invalid option");
  // 创建一个新的table
  lua_newtable(L);  /* table to collect results */
  // 源码方面的信息
  if (strchr(options, 'S')) {
    settabss(L, "source", ar.source);
    settabss(L, "short_src", ar.short_src);
    settabsi(L, "linedefined", ar.linedefined);
    settabsi(L, "lastlinedefined", ar.lastlinedefined);
    settabss(L, "what", ar.what);
  }
  // 当前的行信息
  if (strchr(options, 'l'))
    settabsi(L, "currentline", ar.currentline);
  // 参数信息
  if (strchr(options, 'u')) {
    settabsi(L, "nups", ar.nups);
    settabsi(L, "nparams", ar.nparams);
    settabsb(L, "isvararg", ar.isvararg);
  }
  // 名称信息
  if (strchr(options, 'n')) {
    settabss(L, "name", ar.name);
    settabss(L, "namewhat", ar.namewhat);
  }
  // 是否为尾调用
  if (strchr(options, 't'))
    settabsb(L, "istailcall", ar.istailcall);
  // 激活的行
  if (strchr(options, 'L'))
    treatstackoption(L, L1, "activelines");
  // 函数
  if (strchr(options, 'f'))
    treatstackoption(L, L1, "func");
  return 1;  /* return table */
}

// 得到局部变量
static int db_getlocal (lua_State *L) {
  int arg;
  // 得到对应的线程
  lua_State *L1 = getthread(L, &arg);
  lua_Debug ar;
  const char *name;
  // 局部变量的索引
  int nvar = (int)luaL_checkinteger(L, arg + 2);  /* local-variable index */
  // 是否是函数参数
  if (lua_isfunction(L, arg + 1)) {  /* function argument? */
    // 压入函数
    lua_pushvalue(L, arg + 1);  /* push function */
    // 压入局部变量名称
    lua_pushstring(L, lua_getlocal(L, NULL, nvar));  /* push local name */
    return 1;  /* return only name (there is no value) */
  }
  else {  /* stack-level argument */
    // 栈等级参数
    int level = (int)luaL_checkinteger(L, arg + 1);
    // 得到对应level的堆栈
    if (!lua_getstack(L1, level, &ar))  /* out of range? */
      return luaL_argerror(L, arg+1, "level out of range");
    // 检查堆栈的空间
    checkstack(L, L1, 1);
    // 得到名字
    name = lua_getlocal(L1, &ar, nvar);
    if (name) {
      // 得到值
      lua_xmove(L1, L, 1);  /* move local value */
      // 压入名字
      lua_pushstring(L, name);  /* push name */
      lua_rotate(L, -2, 1);  /* re-order */
      return 2;
    }
    else {
      lua_pushnil(L);  /* no name (nor value) */
      return 1;
    }
  }
}

// 设置局部变量
static int db_setlocal (lua_State *L) {
  int arg;
  const char *name;
  lua_State *L1 = getthread(L, &arg);
  lua_Debug ar;
  // 堆栈的level
  int level = (int)luaL_checkinteger(L, arg + 1);
  // 变量
  int nvar = (int)luaL_checkinteger(L, arg + 2);
  // 得到堆栈信息
  if (!lua_getstack(L1, level, &ar))  /* out of range? */
    return luaL_argerror(L, arg+1, "level out of range");
  luaL_checkany(L, arg+3);
  lua_settop(L, arg+3);
  checkstack(L, L1, 1);
  lua_xmove(L, L1, 1);
  // 设置局部变量
  name = lua_setlocal(L1, &ar, nvar);
  if (name == NULL)
    lua_pop(L1, 1);  /* pop value (if not popped by 'lua_setlocal') */
  lua_pushstring(L, name);
  return 1;
}


/*
** get (if 'get' is true) or set an upvalue from a closure
*/
// 从一个clousure中get(如果'get'是真)或者设置一个upvalue
static int auxupvalue (lua_State *L, int get) {
  const char *name;
  // upvalue的索引
  int n = (int)luaL_checkinteger(L, 2);  /* upvalue index */
  // 第一个参数是否是closure
  luaL_checktype(L, 1, LUA_TFUNCTION);  /* closure */
  // 通过upvalue的索引，得到其值
  name = get ? lua_getupvalue(L, 1, n) : lua_setupvalue(L, 1, n);
  if (name == NULL) return 0;
  // 将值压栈
  lua_pushstring(L, name);
  lua_insert(L, -(get+1));  /* no-op if get is false */
  return get + 1;
}

// 得到upvalue的值
static int db_getupvalue (lua_State *L) {
  return auxupvalue(L, 1);
}

// 设置upvalue的值
static int db_setupvalue (lua_State *L) {
  luaL_checkany(L, 3);
  return auxupvalue(L, 0);
}


/*
** Check whether a given upvalue from a given closure exists and
** returns its index
*/
// 校验一个给定的closure是否有一个给定索引的upvalue，然后返回它的索引
static int checkupval (lua_State *L, int argf, int argnup) {
  // 从指定堆栈索引上，得到upvalue索引
  int nup = (int)luaL_checkinteger(L, argnup);  /* upvalue index */
  // 检查堆栈的argf索引是否是closure
  luaL_checktype(L, argf, LUA_TFUNCTION);  /* closure */
  // 得到对应的upvalue
  luaL_argcheck(L, (lua_getupvalue(L, argf, nup) != NULL), argnup,
                   "invalid upvalue index");
  return nup;
}

// 将upvalue的数据入栈
static int db_upvalueid (lua_State *L) {
  // 从指定堆栈索引上，得到upvalue索引
  int n = checkupval(L, 1, 2);
  // 通过索引得到upvalue的指针，然后压栈
  lua_pushlightuserdata(L, lua_upvalueid(L, 1, n));
  return 1;
}

// 将两个upvalue的值中，n2的值赋给n1
static int db_upvaluejoin (lua_State *L) {
  // 从指定堆栈索引上，得到upvalue索引
  int n1 = checkupval(L, 1, 2);
  // 从指定堆栈索引上，得到upvalue索引
  int n2 = checkupval(L, 3, 4);
  luaL_argcheck(L, !lua_iscfunction(L, 1), 1, "Lua function expected");
  luaL_argcheck(L, !lua_iscfunction(L, 3), 3, "Lua function expected");
  // 赋值
  lua_upvaluejoin(L, 1, n1, 3, n2);
  return 0;
}


/*
** Call hook function registered at hook table for the current
** thread (if there is one)
*/
// 在当前线程中调用当前钩表中注册的钩子函数（如果有的话）
static void hookf (lua_State *L, lua_Debug *ar) {
  static const char *const hooknames[] =
    {"call", "return", "line", "count", "tail call"};
  // 得到注册表中的钩子函数
  lua_rawgetp(L, LUA_REGISTRYINDEX, &HOOKKEY);
  // 压入当前线程
  lua_pushthread(L);
  // 确定有钩子函数
  if (lua_rawget(L, -2) == LUA_TFUNCTION) {  /* is there a hook function? */
    // 压入事件名字
    lua_pushstring(L, hooknames[(int)ar->event]);  /* push event name */
    if (ar->currentline >= 0)
      // 压入当前行
      lua_pushinteger(L, ar->currentline);  /* push current line */
    else lua_pushnil(L);
    lua_assert(lua_getinfo(L, "lS", ar));
    // 调用钩子函数
    lua_call(L, 2, 0);  /* call hook function */
  }
}


/*
** Convert a string mask (for 'sethook') into a bit mask
*/
// 将字符串掩码（用于 'sethook'）转换为位掩码 
static int makemask (const char *smask, int count) {
  int mask = 0;
  if (strchr(smask, 'c')) mask |= LUA_MASKCALL;
  if (strchr(smask, 'r')) mask |= LUA_MASKRET;
  if (strchr(smask, 'l')) mask |= LUA_MASKLINE;
  if (count > 0) mask |= LUA_MASKCOUNT;
  return mask;
}


/*
** Convert a bit mask (for 'gethook') into a string mask
*/
// 将一个位掩码转换进一个字符串
static char *unmakemask (int mask, char *smask) {
  int i = 0;
  if (mask & LUA_MASKCALL) smask[i++] = 'c';
  if (mask & LUA_MASKRET) smask[i++] = 'r';
  if (mask & LUA_MASKLINE) smask[i++] = 'l';
  smask[i] = '\0';
  return smask;
}

// 设置钩子函数
static int db_sethook (lua_State *L) {
  int arg, mask, count;
  lua_Hook func;
  // 得到线程
  lua_State *L1 = getthread(L, &arg);
  // 没有钩子函数
  if (lua_isnoneornil(L, arg+1)) {  /* no hook? */
    // 关闭钩子
    lua_settop(L, arg+1);
    func = NULL; mask = 0; count = 0;  /* turn off hooks */
  }
  else {
    // 得到掩码
    const char *smask = luaL_checkstring(L, arg+2);
    luaL_checktype(L, arg+1, LUA_TFUNCTION);
    count = (int)luaL_optinteger(L, arg + 3, 0);
    // 设置函数和掩码
    func = hookf; mask = makemask(smask, count);
  }
  // 没有钩子函数表
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &HOOKKEY) == LUA_TNIL) {
      // 创建一个钩子函数表
    lua_createtable(L, 0, 2);  /* create a hook table */
    lua_pushvalue(L, -1);
    // 设置注册表中HOOKKEY的值为钩子函数表
    lua_rawsetp(L, LUA_REGISTRYINDEX, &HOOKKEY);  /* set it in position */
    // hooktable.__mode = "k"
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");  /** hooktable.__mode = "k" */
    // 设置hooktable的元表为hooktable
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2);  /* setmetatable(hooktable) = hooktable */
  }
  checkstack(L, L1, 1);
  lua_pushthread(L1); lua_xmove(L1, L, 1);  /* key (thread) */
  // 压栈hook函数
  lua_pushvalue(L, arg + 1);  /* value (hook function) */
  // hooktable[L1] = 新的lua钩子
  lua_rawset(L, -3);  /* hooktable[L1] = new Lua hook */
  // 设置钩子函数
  lua_sethook(L1, func, mask, count);
  return 0;
}

// 得到钩子函数
static int db_gethook (lua_State *L) {
  int arg;
  // 得到对应的线程
  lua_State *L1 = getthread(L, &arg);
  char buff[5];
  // 得到钩子掩码
  int mask = lua_gethookmask(L1);
  // 得到钩子
  lua_Hook hook = lua_gethook(L1);
  if (hook == NULL)  /* no hook? */
    lua_pushnil(L);
  // 外部的钩子函数
  else if (hook != hookf)  /* external hook? */
    lua_pushliteral(L, "external hook");
  // 钩子表必须存在
  else {  /* hook table must exist */
      // 得到钩子表
    lua_rawgetp(L, LUA_REGISTRYINDEX, &HOOKKEY);
    checkstack(L, L1, 1);
    lua_pushthread(L1); lua_xmove(L1, L, 1);
    // 第一个结果：result = hooktable[L1]
    lua_rawget(L, -2);   /* 1st result = hooktable[L1] */
    lua_remove(L, -2);  /* remove hook table */
  }
  // 第二个结果： result = mask
  lua_pushstring(L, unmakemask(mask, buff));  /* 2nd result = mask */
  // 第三个结果： result = count
  lua_pushinteger(L, lua_gethookcount(L1));  /* 3rd result = count */
  return 3;
}

// 调试信息
static int db_debug (lua_State *L) {
  for (;;) {
    char buffer[250];
    // 写入error
    lua_writestringerror("%s", "lua_debug> ");
    // 如果标准输入的为0，或者是cont，直接返回
    if (fgets(buffer, sizeof(buffer), stdin) == 0 ||
        strcmp(buffer, "cont\n") == 0)
      return 0;
    // 加载标准输入得到的字符串，然后调用
    if (luaL_loadbuffer(L, buffer, strlen(buffer), "=(debug command)") ||
        lua_pcall(L, 0, 0, 0))
      lua_writestringerror("%s\n", lua_tostring(L, -1));
    // 删除返回值
    lua_settop(L, 0);  /* remove eventual returns */
  }
}

// 堆栈回溯
static int db_traceback (lua_State *L) {
  int arg;
  lua_State *L1 = getthread(L, &arg);
  const char *msg = lua_tostring(L, arg + 1);
  // 非字符串的msg
  if (msg == NULL && !lua_isnoneornil(L, arg + 1))  /* non-string 'msg'? */
    lua_pushvalue(L, arg + 1);  /* return it untouched */
  else {
    // 堆栈的level
    int level = (int)luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
    // 进行堆栈回溯
    luaL_traceback(L, L1, msg, level);
  }
  return 1;
}


static const luaL_Reg dblib[] = {
  {"debug", db_debug},
  {"getuservalue", db_getuservalue},
  {"gethook", db_gethook},
  {"getinfo", db_getinfo},
  {"getlocal", db_getlocal},
  {"getregistry", db_getregistry},
  {"getmetatable", db_getmetatable},
  {"getupvalue", db_getupvalue},
  {"upvaluejoin", db_upvaluejoin},
  {"upvalueid", db_upvalueid},
  {"setuservalue", db_setuservalue},
  {"sethook", db_sethook},
  {"setlocal", db_setlocal},
  {"setmetatable", db_setmetatable},
  {"setupvalue", db_setupvalue},
  {"traceback", db_traceback},
  {NULL, NULL}
};


LUAMOD_API int luaopen_debug (lua_State *L) {
  luaL_newlib(L, dblib);
  return 1;
}

