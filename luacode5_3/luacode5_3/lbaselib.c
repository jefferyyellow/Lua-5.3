/*
** $Id: lbaselib.c,v 1.314.1.1 2017/04/19 17:39:34 roberto Exp $
** Basic library
** See Copyright Notice in lua.h
*/

#define lbaselib_c
#define LUA_LIB

#include "lprefix.h"


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


static int luaB_print (lua_State *L) {
  int n = lua_gettop(L);  /* number of arguments */
  int i;
  lua_getglobal(L, "tostring");
  for (i=1; i<=n; i++) {
    const char *s;
    size_t l;
    lua_pushvalue(L, -1);  /* function to be called */
    lua_pushvalue(L, i);   /* value to print */
    lua_call(L, 1, 1);
    s = lua_tolstring(L, -1, &l);  /* get result */
    if (s == NULL)
      return luaL_error(L, "'tostring' must return a string to 'print'");
    if (i>1) lua_writestring("\t", 1);
    lua_writestring(s, l);
    lua_pop(L, 1);  /* pop result */
  }
  lua_writeline();
  return 0;
}


#define SPACECHARS	" \f\n\r\t\v"

static const char *b_str2int (const char *s, int base, lua_Integer *pn) {
  lua_Unsigned n = 0;
  int neg = 0;
  s += strspn(s, SPACECHARS);  /* skip initial spaces */
  if (*s == '-') { s++; neg = 1; }  /* handle signal */
  else if (*s == '+') s++;
  if (!isalnum((unsigned char)*s))  /* no digit? */
    return NULL;
  do {
    int digit = (isdigit((unsigned char)*s)) ? *s - '0'
                   : (toupper((unsigned char)*s) - 'A') + 10;
    if (digit >= base) return NULL;  /* invalid numeral */
    n = n * base + digit;
    s++;
  } while (isalnum((unsigned char)*s));
  s += strspn(s, SPACECHARS);  /* skip trailing spaces */
  *pn = (lua_Integer)((neg) ? (0u - n) : n);
  return s;
}


static int luaB_tonumber (lua_State *L) {
  if (lua_isnoneornil(L, 2)) {  /* standard conversion? */
    luaL_checkany(L, 1);
    if (lua_type(L, 1) == LUA_TNUMBER) {  /* already a number? */
      lua_settop(L, 1);  /* yes; return it */
      return 1;
    }
    else {
      size_t l;
      const char *s = lua_tolstring(L, 1, &l);
      if (s != NULL && lua_stringtonumber(L, s) == l + 1)
        return 1;  /* successful conversion to number */
      /* else not a number */
    }
  }
  else {
    size_t l;
    const char *s;
    lua_Integer n = 0;  /* to avoid warnings */
    lua_Integer base = luaL_checkinteger(L, 2);
    luaL_checktype(L, 1, LUA_TSTRING);  /* no numbers as strings */
    s = lua_tolstring(L, 1, &l);
    luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
    if (b_str2int(s, (int)base, &n) == s + l) {
      lua_pushinteger(L, n);
      return 1;
    }  /* else not a number */
  }  /* else not a number */
  lua_pushnil(L);  /* not a number */
  return 1;
}


static int luaB_error (lua_State *L) {
  int level = (int)luaL_optinteger(L, 2, 1);
  lua_settop(L, 1);
  if (lua_type(L, 1) == LUA_TSTRING && level > 0) {
    luaL_where(L, level);   /* add extra information */
    lua_pushvalue(L, 1);
    lua_concat(L, 2);
  }
  return lua_error(L);
}


static int luaB_getmetatable (lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_getmetatable(L, 1)) {
    lua_pushnil(L);
    return 1;  /* no metatable */
  }
  luaL_getmetafield(L, 1, "__metatable");
  return 1;  /* returns either __metatable field (if present) or metatable */
}


static int luaB_setmetatable (lua_State *L) {
  int t = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_argcheck(L, t == LUA_TNIL || t == LUA_TTABLE, 2,
                    "nil or table expected");
  if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
    return luaL_error(L, "cannot change a protected metatable");
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;
}


static int luaB_rawequal (lua_State *L) {
  luaL_checkany(L, 1);
  luaL_checkany(L, 2);
  lua_pushboolean(L, lua_rawequal(L, 1, 2));
  return 1;
}


static int luaB_rawlen (lua_State *L) {
  int t = lua_type(L, 1);
  luaL_argcheck(L, t == LUA_TTABLE || t == LUA_TSTRING, 1,
                   "table or string expected");
  lua_pushinteger(L, lua_rawlen(L, 1));
  return 1;
}


static int luaB_rawget (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  lua_rawget(L, 1);
  return 1;
}

static int luaB_rawset (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  luaL_checkany(L, 3);
  lua_settop(L, 3);
  lua_rawset(L, 1);
  return 1;
}


static int luaB_collectgarbage (lua_State *L) {
  static const char *const opts[] = {"stop", "restart", "collect",
    "count", "step", "setpause", "setstepmul",
    "isrunning", NULL};
  static const int optsnum[] = {LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT,
    LUA_GCCOUNT, LUA_GCSTEP, LUA_GCSETPAUSE, LUA_GCSETSTEPMUL,
    LUA_GCISRUNNING};
  int o = optsnum[luaL_checkoption(L, 1, "collect", opts)];
  int ex = (int)luaL_optinteger(L, 2, 0);
  int res = lua_gc(L, o, ex);
  switch (o) {
    case LUA_GCCOUNT: {
      int b = lua_gc(L, LUA_GCCOUNTB, 0);
      lua_pushnumber(L, (lua_Number)res + ((lua_Number)b/1024));
      return 1;
    }
    case LUA_GCSTEP: case LUA_GCISRUNNING: {
      lua_pushboolean(L, res);
      return 1;
    }
    default: {
      lua_pushinteger(L, res);
      return 1;
    }
  }
}


static int luaB_type (lua_State *L) {
  int t = lua_type(L, 1);
  luaL_argcheck(L, t != LUA_TNONE, 1, "value expected");
  lua_pushstring(L, lua_typename(L, t));
  return 1;
}


static int pairsmeta (lua_State *L, const char *method, int iszero,
                      lua_CFunction iter) {
  luaL_checkany(L, 1);
  if (luaL_getmetafield(L, 1, method) == LUA_TNIL) {  /* no metamethod? */
    lua_pushcfunction(L, iter);  /* will return generator, */
    lua_pushvalue(L, 1);  /* state, */
    if (iszero) lua_pushinteger(L, 0);  /* and initial value */
    else lua_pushnil(L);
  }
  else {
    lua_pushvalue(L, 1);  /* argument 'self' to metamethod */
    lua_call(L, 1, 3);  /* get 3 values from metamethod */
  }
  return 3;
}

// 下一个
static int luaB_next (lua_State *L) {
    // 第一个参数是表
  luaL_checktype(L, 1, LUA_TTABLE);
  // 如果没有就创建第二个参数
  lua_settop(L, 2);  /* create a 2nd argument if there isn't one */
  // 下一个
  if (lua_next(L, 1))
    return 2;
  else {
    lua_pushnil(L);
    return 1;
  }
}


static int luaB_pairs (lua_State *L) {
  return pairsmeta(L, "__pairs", 0, luaB_next);
}


/*
** Traversal function for 'ipairs'
*/
// ipairs的遍历函数
static int ipairsaux (lua_State *L) {
  // 得到第二个参数，然后加1
  lua_Integer i = luaL_checkinteger(L, 2) + 1;
  // 压入参数
  lua_pushinteger(L, i);
  return (lua_geti(L, 1, i) == LUA_TNIL) ? 1 : 2;
}


/*
** 'ipairs' function. Returns 'ipairsaux', given "table", 0.
** (The given "table" may not be a table.)
*/
// 'ipairs'函数。返回给定表的‘ipairsaux’，0.
// 该给定“表”可能不是表
static int luaB_ipairs (lua_State *L) {
#if defined(LUA_COMPAT_IPAIRS)
  return pairsmeta(L, "__ipairs", 1, ipairsaux);
#else
  // 检查确定有一个参数
  luaL_checkany(L, 1);
  // 将辅助函数压参
  lua_pushcfunction(L, ipairsaux);  /* iteration function */
  // 将lua_State压参
  lua_pushvalue(L, 1);  /* state */
  // 还要初始化值
  lua_pushinteger(L, 0);  /* initial value */
  return 3;
#endif
}


// 加载辅助函数
static int load_aux (lua_State *L, int status, int envidx) {
    // 加载正确
  if (status == LUA_OK) {
    // ‘环境’参数
    if (envidx != 0) {  /* 'env' parameter? */
      // 压入环境索引
      lua_pushvalue(L, envidx);  /* environment for loaded function */
      if (!lua_setupvalue(L, -2, 1))  /* set it as 1st upvalue */
        lua_pop(L, 1);  /* remove 'env' if not used by previous call */
    }
    return 1;
  }
  // 加载出错，错误信息在堆栈顶部
  else {  /* error (message is on top of the stack) */
    lua_pushnil(L);
    // 将nil放在堆栈顶部的前面
    lua_insert(L, -2);  /* put before error message */
    return 2;  /* return nil plus error message */
  }
}

// 加载文件
static int luaB_loadfile (lua_State *L) {
    // 第一个文件名
  const char *fname = luaL_optstring(L, 1, NULL);
  // 第二个模式
  const char *mode = luaL_optstring(L, 2, NULL);
  // 环境索引
  int env = (!lua_isnone(L, 3) ? 3 : 0);  /* 'env' index or 0 if no 'env' */
  int status = luaL_loadfilex(L, fname, mode);
  return load_aux(L, status, env);
}


/*
** {======================================================
** Generic Read function
** =======================================================
*/


/*
** reserved slot, above all arguments, to hold a copy of the returned
** string to avoid it being collected while parsed. 'load' has four
** optional arguments (chunk, source name, mode, and environment).
*/
// 预留的插槽，在所有的参数上面，用于保存返回的副本字符串，以避免在解析时被收集。
// “负载”有四个可选参数（块，源名称，模式和环境）
#define RESERVEDSLOT	5


/*
** Reader for generic 'load' function: 'lua_load' uses the
** stack for internal stuff, so the reader cannot change the
** stack top. Instead, it keeps its resulting string in a
** reserved slot inside the stack.
*/
// 一般load函数的Reader：lua_load使用内部stuff的堆栈，所以reader不能改变堆栈顶部。
// 相反，而是将其结果字符串保存在堆栈内部的预留插槽。
static const char *generic_reader (lua_State *L, void *ud, size_t *size) {
  (void)(ud);  /* not used */
  // 保证堆栈上有两个空位
  luaL_checkstack(L, 2, "too many nested functions");
  // 得到值
  lua_pushvalue(L, 1);  /* get function */
  // 调用
  lua_call(L, 0, 1);  /* call it */
  // 如果栈顶是nil
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);  /* pop result */
    *size = 0;
    return NULL;
  }
  // 如果有值，只能是字符串
  else if (!lua_isstring(L, -1))
    luaL_error(L, "reader function must return a string");
  // 将栈顶的字符串替换到预留的slot
  lua_replace(L, RESERVEDSLOT);  /* save string in reserved slot */
  // 返回加载的字符串
  return lua_tolstring(L, RESERVEDSLOT, size);
}

// load的本质就是在Lua代码中运行一段存储在字符串中的代码
static int luaB_load (lua_State *L) {
  int status;
  size_t l;
  // 得到索引为1的字符串，将长度存在l中
  const char *s = lua_tolstring(L, 1, &l);
  // 得到索引为3的字符串，若字符串不存在或者为nil，使用默认的模式"bt"
  const char *mode = luaL_optstring(L, 3, "bt");
  // 得到环境索引
  int env = (!lua_isnone(L, 4) ? 4 : 0);  /* 'env' index or 0 if no 'env' */
  // 如果字符串不为空
  if (s != NULL) {  /* loading a string? */
    // 取出块的名字
    const char *chunkname = luaL_optstring(L, 2, s);
    // 加载
    status = luaL_loadbufferx(L, s, l, chunkname, mode);
  }
  // 从一个reader函数中加载
  else {  /* loading from a reader function */
     // 块名为"=(load)"
    const char *chunkname = luaL_optstring(L, 2, "=(load)");
    // index为1的是函数
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, RESERVEDSLOT);  /* create reserved slot */
    // 加载
    status = lua_load(L, generic_reader, NULL, chunkname, mode);
  }
  return load_aux(L, status, env);
}

/* }====================================================== */

// 返回参数
static int dofilecont (lua_State *L, int d1, lua_KContext d2) {
  (void)d1;  (void)d2;  /* only to match 'lua_Kfunction' prototype */
  return lua_gettop(L) - 1;
}

// 加载文件并且执行文件中的代码
static int luaB_dofile (lua_State *L) {
    // 文件名，luaL_optstring：如果函数的第 arg 个参数是一个 字符串，返回该字符串。 若该参数不存在或是 nil，
  const char *fname = luaL_optstring(L, 1, NULL);
  // 只保留这个文件名的参数
  lua_settop(L, 1);
  // 加载文件
  if (luaL_loadfile(L, fname) != LUA_OK)
    return lua_error(L);
  // 执行代码
  lua_callk(L, 0, LUA_MULTRET, 0, dofilecont);
  return dofilecont(L, 0, 0);
}

// 断言
static int luaB_assert (lua_State *L) {
  // 将第一个参数转换成bool，如果是true，就返回所有参数
  if (lua_toboolean(L, 1))  /* condition is true? */
    return lua_gettop(L);  /* return all arguments */
  // 出错了
  else {  /* error */
    // 这个必须是个条件
    luaL_checkany(L, 1);  /* there must be a condition */
    // 将条件删除了
    lua_remove(L, 1);  /* remove it */
    // 压参“assertion failed!”的提示信息
    lua_pushliteral(L, "assertion failed!");  /* default message */
    // 只留下错误消息
    lua_settop(L, 1);  /* leave only message (default if no other one) */
    // 调用错误函数
    return luaB_error(L);  /* call 'error' */
  }
}

// select(n, ...)  --数字n表示起点，select(n, ...)返回从起点n到结束的可变参数，比如：
// n = 3，... 是 0，1，2，3，4，5
// 则 select(n, ...) 就表示...中从第3个到最后一个的多个数：2，3，4，5。并且2，3，4，5是4个数，不是列表或其他的数据结构
// 所以， 下面的代码中，a = select(3, ...) 就表示的是  a = 2, 3, 4, 5。所以，a = 2;
static int luaB_select (lua_State *L) {
  int n = lua_gettop(L);
 // 如果第一个参数是#，那就返回可变参数的数目，n-1中的-1表示将#这个参数排除
  if (lua_type(L, 1) == LUA_TSTRING && *lua_tostring(L, 1) == '#') {
    lua_pushinteger(L, n-1);
    return 1;
  }
  else {
      // 取得第一个参数，并转换成整数
    lua_Integer i = luaL_checkinteger(L, 1);
    // 从栈顶开始算
    if (i < 0) i = n + i;
    // 从栈底开始算
    else if (i > n) i = n;
    // 范围检查
    luaL_argcheck(L, 1 <= i, 1, "index out of range");
    // 其实不需要改变堆栈，只需要返回对应的堆栈上的栈顶的数个值
    return n - (int)i;
  }
}


/*
** Continuation function for 'pcall' and 'xpcall'. Both functions
** already pushed a 'true' before doing the call, so in case of success
** 'finishpcall' only has to return everything in the stack minus
** 'extra' values (where 'extra' is exactly the number of items to be
** ignored).
*/
// “ pcall”和“ xpcall”的延续功能。 两种功能在调用之前就已经压入了“ true”，因此在成功的情况下
// 'finishpcall'只需要返回堆栈中的所有内容减去'extra'值（其中'extra'恰好是要忽略）。
static int finishpcall (lua_State *L, int status, lua_KContext extra) {
    // 如果不是成功或者暂停，就是出错了
  if (status != LUA_OK && status != LUA_YIELD) {  /* error? */
    // 压入false
    lua_pushboolean(L, 0);  /* first result (false) */
    // 和出错的消息
    lua_pushvalue(L, -2);  /* error message */
    return 2;  /* return false, msg */
  }
  else
    // 返回所有的结果，就是堆栈上的值数目坚强额外的
    return lua_gettop(L) - (int)extra;  /* return all results */
}


static int luaB_pcall (lua_State *L) {
  int status;
  // 堆栈索引1的位置是否有参数
  luaL_checkany(L, 1);
  // 压入一个true
  lua_pushboolean(L, 1);  /* first result if no errors */
  // 将栈顶的bool值插入堆栈索引1的位置
  lua_insert(L, 1);  /* put it in place */
  status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finishpcall);
  return finishpcall(L, status, 0);
}


/*
** Do a protected call with error handling. After 'lua_rotate', the
** stack will have <f, err, true, f, [args...]>; so, the function passes
** 2 to 'finishpcall' to skip the 2 first values when returning results.
*/
// 使用错误处理保护的call。 在“ lua_rotate”之后，
// 堆栈将具有<f，err，true，f，[args ...]>; 因此，该函数通过了
// 2到“ finishpcall”以返回结果时跳过前两个值。
static int luaB_xpcall (lua_State *L) {
  int status;
  // 在堆栈顶部得到参数个数
  int n = lua_gettop(L);
  // 第二个参数是否为函数（错误处理函数）
  luaL_checktype(L, 2, LUA_TFUNCTION);  /* check error function */
  // 返回结果
  lua_pushboolean(L, 1);  /* first result */
  // 函数值
  lua_pushvalue(L, 1);  /* function */

  // <f, err, [args...],true, f>;通过lua_rotate(L, 3, 2)后，将最后的2个看成一个整体，
  // 插入堆栈索引为3的位置，就变成了<f, err, true, f,[args...]>;
  lua_rotate(L, 3, 2);  /* move them below function's arguments */
  // 调用函数,注意，这里的参数变成了n-2,就是将原来的f和err两个函数度过滤掉
  status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishpcall);
  return finishpcall(L, status, 2);
}


static int luaB_tostring (lua_State *L) {
  // 检查堆栈索引1的类型
  luaL_checkany(L, 1);
  // 将堆栈索引1的值转换成字符串然后压栈
  luaL_tolstring(L, 1, NULL);
  return 1;
}


static const luaL_Reg base_funcs[] = {
  {"assert", luaB_assert},
  {"collectgarbage", luaB_collectgarbage},
  {"dofile", luaB_dofile},
  {"error", luaB_error},
  {"getmetatable", luaB_getmetatable},
  {"ipairs", luaB_ipairs},
  {"loadfile", luaB_loadfile},
  {"load", luaB_load},
#if defined(LUA_COMPAT_LOADSTRING)
  {"loadstring", luaB_load},
#endif
  {"next", luaB_next},
  {"pairs", luaB_pairs},
  {"pcall", luaB_pcall},
  {"print", luaB_print},
  {"rawequal", luaB_rawequal},
  {"rawlen", luaB_rawlen},
  {"rawget", luaB_rawget},
  {"rawset", luaB_rawset},
  {"select", luaB_select},
  {"setmetatable", luaB_setmetatable},
  {"tonumber", luaB_tonumber},
  {"tostring", luaB_tostring},
  {"type", luaB_type},
  {"xpcall", luaB_xpcall},
  /* placeholders */
  {"_G", NULL},
  {"_VERSION", NULL},
  {NULL, NULL}
};


LUAMOD_API int luaopen_base (lua_State *L) {
  /* open lib into global table */
   // 得到全局表放在栈顶
  lua_pushglobaltable(L);
  // 将base_funcs注册在全局表中
  luaL_setfuncs(L, base_funcs, 0);
  /* set global _G */
  // 将全局表中的_G设置为-1
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_G");
  /* set global _VERSION */
  // 设置全局表中的_VERSION 为LUA_VERSION的值
  lua_pushliteral(L, LUA_VERSION);
  lua_setfield(L, -2, "_VERSION");
  return 1;
}

