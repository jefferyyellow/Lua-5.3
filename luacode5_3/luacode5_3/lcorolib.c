/*
** $Id: lcorolib.c,v 1.10.1.1 2017/04/19 17:20:42 roberto Exp $
** Coroutine Library
** See Copyright Notice in lua.h
*/

#define lcorolib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

// 得到第一个参数是否是一个协程，如果是返回协程，不是返回NULL
static lua_State *getco (lua_State *L) {
    // 
  lua_State *co = lua_tothread(L, 1);
  luaL_argcheck(L, co, 1, "thread expected");
  return co;
}

// 恢复执行的辅助函数
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status;
  // 堆栈上参数空间大小校验
  if (!lua_checkstack(co, narg)) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  // 状态为OK，并且堆栈为空
  if (lua_status(co) == LUA_OK && lua_gettop(co) == 0) {
    lua_pushliteral(L, "cannot resume dead coroutine");
    return -1;  /* error flag */
  }
    // 将narg个参数从L移动到co协程
  lua_xmove(L, co, narg);
  // 恢复执行
  status = lua_resume(co, L, narg);
    // 
  if (status == LUA_OK || status == LUA_YIELD) {
    // 得到恢复执行的协程堆栈
    int nres = lua_gettop(co);
    // 检查L的堆栈是否能装得下返回值
    if (!lua_checkstack(L, nres + 1)) {
      // 将返回值去掉
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    // 如果resume成功，需要将yielded的值拷贝到调用resume的协程
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  // 出错
  else {
    // 将错误消息从co移动到L
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}

// 协程恢复执行
static int luaB_coresume (lua_State *L) {
  // 得到栈顶的协程
  lua_State *co = getco(L);
  int r;
  // 执行恢复辅助函数
  r = auxresume(L, co, lua_gettop(L) - 1);
  // 根据auxresume的返回值来做不同的处理。当返回值小于0时，说明resume操作出错，并
  // 且此时出错信息在栈顶，因此压入false以及出错消息；否则，auxresume的返回值表示
  // 执行resume操作时返回的参数数量，这种情况下压人true以及这些返回参数。
  if (r < 0) {
    // 返回false
    lua_pushboolean(L, 0);
    // 错误信息
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    // 返回成功
    lua_pushboolean(L, 1);
    // 返回参数数目：恢复执行的返回值数目+true
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}

// wrap的辅助函数
static int luaB_auxwrap (lua_State *L) {
  // 先获取协程
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));
  // 恢复执行
  int r = auxresume(L, co, lua_gettop(L));
  if (r < 0) {
      // 出错了，如果有错误信息
    if (lua_type(L, -1) == LUA_TSTRING) {  /* error object is a string? */
      // 找到出错的地方
      luaL_where(L, 1);  /* add extra info */
      // 插入出错信息
      lua_insert(L, -2);
      // 连接出错地方的信息和出错原因
      lua_concat(L, 2);
    }
    return lua_error(L);  /* propagate error */
  }
  return r;
}

// 创建一个协程
static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  // 第一个参数是函数
  luaL_checktype(L, 1, LUA_TFUNCTION);
  // 创建新协程
  NL = lua_newthread(L);
  // 重新将函数入栈
  lua_pushvalue(L, 1);  /* move function to top */
  // 将L堆栈上得数据移动到NL堆栈上
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}

// coroutine.wrap (f)
// 创建一个主体函数为 f 的新协程。 f 必须是一个 Lua 的函数。 返回一个函数， 每次调用该函数都会延续该协程。 
// 传给这个函数的参数都会作为 resume 的额外参数。 和 resume 返回相同的值， 只是没有第一个布尔量。 如果发生任何错误，抛出这个错误。
static int luaB_cowrap (lua_State *L) {
  // 创建协程
  luaB_cocreate(L);
  // 压入辅助函数
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}


// 挂起正在调用的协程的执行。 传递给 yield 的参数都会转为 resume 的额外返回值。
static int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}

// 以字符串形式返回协程 co 的状态： 
// 如果协程正在运行（它就是调用 status 的那个） ，返回 "running"； 
// 如果协程调用 yield 挂起或是还没有开始运行，返回 "suspended"； 
// 如果协程是活动的，都并不在运行（即它正在延续其它协程），返回 "normal"； 
// 如果协程运行完主体函数或因错误停止，返回 "dead"。
static int luaB_costatus (lua_State *L) {
  lua_State *co = getco(L);
  // 当前协程，直接返回运行中
  if (L == co) lua_pushliteral(L, "running");
  else {
    switch (lua_status(co)) {
      case LUA_YIELD:
        lua_pushliteral(L, "suspended");
        break;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar) > 0)  /* does it have frames? */
          lua_pushliteral(L, "normal");  /* it is running */
        else if (lua_gettop(co) == 0)
            lua_pushliteral(L, "dead");
        else
          lua_pushliteral(L, "suspended");  /* initial state */
        break;
      }
      default:  /* some error occurred */
        lua_pushliteral(L, "dead");
        break;
    }
  }
  return 1;
}

// 如果正在运行的协程可以让出，则返回真。
// 不在主线程中或不在一个无法让出的 C 函数中时，当前协程是可让出的
static int luaB_yieldable (lua_State *L) {
  lua_pushboolean(L, lua_isyieldable(L));
  return 1;
}

// 返回当前正在运行的协程加一个布尔量。 如果当前运行的协程是主线程，其为真。
static int luaB_corunning (lua_State *L) {
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}


static const luaL_Reg co_funcs[] = {
  {"create", luaB_cocreate},
  {"resume", luaB_coresume},
  {"running", luaB_corunning},
  {"status", luaB_costatus},
  {"wrap", luaB_cowrap},
  {"yield", luaB_yield},
  {"isyieldable", luaB_yieldable},
  {NULL, NULL}
};



LUAMOD_API int luaopen_coroutine (lua_State *L) {
  luaL_newlib(L, co_funcs);
  return 1;
}

