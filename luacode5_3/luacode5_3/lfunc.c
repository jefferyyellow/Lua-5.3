/*
** $Id: lfunc.c,v 2.45.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#define lfunc_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"

// 创建一个新的C闭包
CClosure *luaF_newCclosure (lua_State *L, int n) {
  // 创建一个新的C闭包
  GCObject *o = luaC_newobj(L, LUA_TCCL, sizeCclosure(n));
  CClosure *c = gco2ccl(o);
  // 设置n个upvalues
  c->nupvalues = cast_byte(n);
  return c;
}

// 创建一个新的lua闭包
LClosure *luaF_newLclosure (lua_State *L, int n) {
  GCObject *o = luaC_newobj(L, LUA_TLCL, sizeLclosure(n));
  LClosure *c = gco2lcl(o);
  c->p = NULL;
  c->nupvalues = cast_byte(n);
  while (n--) c->upvals[n] = NULL;
  return c;
}

/*
** fill a closure with new closed upvalues
*/
// 使用新的upvalues来填充closure
void luaF_initupvals (lua_State *L, LClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++) {
    // 创建一个upvalue
    UpVal *uv = luaM_new(L, UpVal);
    // 设置引用计数
    uv->refcount = 1;
    // 都是close状态
    uv->v = &uv->u.value;  /* make it closed */
    setnilvalue(uv->v);
    // 将upvalue关联到闭包上
    cl->upvals[i] = uv;
  }
}

// 查找upvalues，如果没有对应的upvalues就会创建一个新的
UpVal *luaF_findupval (lua_State *L, StkId level) {
  UpVal **pp = &L->openupval;
  UpVal *p;
  UpVal *uv;
  lua_assert(isintwups(L) || L->openupval == NULL);
  while (*pp != NULL && (p = *pp)->v >= level) {
    lua_assert(upisopen(p));
    // 找到对应的upvalue
    if (p->v == level)  /* found a corresponding upvalue? */
      return p;  /* return it */
    pp = &p->u.open.next;
  }
  /* not found: create a new upvalue */
  // 没找到就新建一个
  uv = luaM_new(L, UpVal);
  uv->refcount = 0;
  // 链接到open upvalues列表
  uv->u.open.next = *pp;  /* link it to list of open upvalues */
  uv->u.open.touched = 1;
  *pp = uv;
  // 存在于堆栈中的当前值
  uv->v = level;  /* current value lives in the stack */
  if (!isintwups(L)) {  /* thread not in list of threads with upvalues? */
    L->twups = G(L)->twups;  /* link it to the list */
    G(L)->twups = L;
  }
  return uv;
}

// 当离开一个代码块后，这个代码块中定义的局部变量就变为不可见的。Lua会调整数据栈指针，销毁掉这些变量。
// 若这些栈值还被某些闭包以open状态的upvalue的形式引用，就需要把它们关闭。
// luaF_close函数逻辑：先将当前UpVal从L->openipval链表中剔除掉，然后判断当前UpVal->refcount查看是否还有被其他闭包引用, 
// 如果refcount == 0 则释放UpVal 结构；如果还有引用则需要把数据（uv->v 这时候在数据栈上）从数据栈上copy到UpVal结构中的
// （uv->u.value）中，最后修正UpVal中的指针 v（uv->v现在指向UpVal结构中 uv->u.value所在地址）。

// 处理函数中的upvalue，如果引用计数为0，释放，或者放入slot
void luaF_close (lua_State *L, StkId level) {
  UpVal *uv;
  while (L->openupval != NULL && (uv = L->openupval)->v >= level) {
    lua_assert(upisopen(uv));
    // 从链表中移除
    L->openupval = uv->u.open.next;  /* remove from 'open' list */
    // 引用计数为0
    if (uv->refcount == 0)  /* no references? */
      // 释放
      luaM_free(L, uv);  /* free upvalue */
    else {
      setobj(L, &uv->u.value, uv->v);  /* move value to upvalue slot */
      uv->v = &uv->u.value;  /* now current value lives here */
      luaC_upvalbarrier(L, uv);
    }
  }
}

// 创建一个新的函数原型，并初始化
Proto *luaF_newproto (lua_State *L) {
  GCObject *o = luaC_newobj(L, LUA_TPROTO, sizeof(Proto));
  Proto *f = gco2p(o);
  f->k = NULL;
  f->sizek = 0;
  f->p = NULL;
  f->sizep = 0;
  f->code = NULL;
  f->cache = NULL;
  f->sizecode = 0;
  f->lineinfo = NULL;
  f->sizelineinfo = 0;
  f->upvalues = NULL;
  f->sizeupvalues = 0;
  f->numparams = 0;
  f->is_vararg = 0;
  f->maxstacksize = 0;
  f->locvars = NULL;
  f->sizelocvars = 0;
  f->linedefined = 0;
  f->lastlinedefined = 0;
  f->source = NULL;
  return f;
}

// 释放函数原型
void luaF_freeproto (lua_State *L, Proto *f) {
  luaM_freearray(L, f->code, f->sizecode);
  luaM_freearray(L, f->p, f->sizep);
  luaM_freearray(L, f->k, f->sizek);
  luaM_freearray(L, f->lineinfo, f->sizelineinfo);
  luaM_freearray(L, f->locvars, f->sizelocvars);
  luaM_freearray(L, f->upvalues, f->sizeupvalues);
  luaM_free(L, f);
}


/*
** Look for n-th local variable at line 'line' in function 'func'.
** Returns NULL if not found.
*/
// 在函数 'func' 的第 'line' 行查找第 n 个局部变量。如果未找到，则返回 NULL。
const char *luaF_getlocalname (const Proto *f, int local_number, int pc) {
  int i;
  for (i = 0; i<f->sizelocvars && f->locvars[i].startpc <= pc; i++) {
    if (pc < f->locvars[i].endpc) {  /* is variable active? */
      local_number--;
      if (local_number == 0)
        // 返回变量名
        return getstr(f->locvars[i].varname);
    }
  }
  return NULL;  /* not found */
}

