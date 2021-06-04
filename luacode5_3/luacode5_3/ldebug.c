/*
** $Id: ldebug.c,v 2.121.1.2 2017/07/10 17:21:50 roberto Exp $
** Debug Interface
** See Copyright Notice in lua.h
*/

#define ldebug_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"


// 不是lua闭包和轻量级C函数
#define noLuaClosure(f)		((f) == NULL || (f)->c.tt == LUA_TCCL)


/* Active Lua function (given call info) */
// 激活的lua函数，（给定的调用信息)
#define ci_func(ci)		(clLvalue((ci)->func))


static const char *funcnamefromcode (lua_State *L, CallInfo *ci,
                                    const char **name);

// 得到执行指令的地址（当前指令到函数开始地址的距离）
static int currentpc (CallInfo *ci) {
  lua_assert(isLua(ci));
  // 返回当前指令到函数开始地址的距离
  return pcRel(ci->u.l.savedpc, ci_func(ci)->p);
}

// 得到源码的行信息
static int currentline (CallInfo *ci) {
  return getfuncline(ci_func(ci)->p, currentpc(ci));
}


/*
** If function yielded, its 'func' can be in the 'extra' field. The
** next function restores 'func' to its correct value for debugging
** purposes. (It exchanges 'func' and 'extra'; so, when called again,
** after debugging, it also "re-restores" ** 'func' to its altered value.
*/
// 如果函数暂停，它的func放在extra字段中，为了调试的目的，下一个函数重新恢复func的值
// （它交换“ func”和“ extra”；因此，当再次调用时，调试后，它还会将**'func'“恢复”到其更改后的值。
static void swapextra (lua_State *L) {
  if (L->status == LUA_YIELD) {
	// 得到当前函数的调用信息
    CallInfo *ci = L->ci;  /* get function that yielded */
	// 交换ci->func和ci->extra
    StkId temp = ci->func;  /* exchange its 'func' and 'extra' values */
    ci->func = restorestack(L, ci->extra);
    ci->extra = savestack(L, temp);
  }
}


/*
** This function can be called asynchronously (e.g. during a signal).
** Fields 'oldpc', 'basehookcount', and 'hookcount' (set by
** 'resethookcount') are for debug only, and it is no problem if they
** get arbitrary values (causes at most one wrong hook call). 'hookmask'
** is an atomic value. We assume that pointers are atomic too (e.g., gcc
** ensures that for all platforms where it runs). Moreover, 'hook' is
** always checked before being called (see 'luaD_hook').
*/
// 可以异步调用此函数（例如，在信号期间）。字段“oldpc”、“basehookcount”和
// “hookcount”(由'resethookcount'设置)只是为了调试，并且不管他们得到了一个什么样的值都没有问题
// （最多只会导致一个错误的钩子函数的调用）。'hookmask'是一个原子值。我们假定指针也是原子的（例如：
// gcc保证适用于它运行的所有平台）。此外，'hook' 在被调用之前总是被检查（参见 'luaD_hook'）。 
// 设置钩子
LUA_API void lua_sethook (lua_State *L, lua_Hook func, int mask, int count) {
  if (func == NULL || mask == 0) {  /* turn off hooks? */
    mask = 0;
    func = NULL;
  }
  // 保存老的pc
  if (isLua(L->ci))
    L->oldpc = L->ci->u.l.savedpc;
  // 设置钩子函数和次数
  L->hook = func;
  L->basehookcount = count;
  resethookcount(L);
  L->hookmask = cast_byte(mask);
}

// 得到钩子
LUA_API lua_Hook lua_gethook (lua_State *L) {
  return L->hook;
}

// 得到钩子的掩码
LUA_API int lua_gethookmask (lua_State *L) {
  return L->hookmask;
}

// 得到钩子数量
LUA_API int lua_gethookcount (lua_State *L) {
  return L->basehookcount;
}

// 得到指定level的堆栈信息
LUA_API int lua_getstack (lua_State *L, int level, lua_Debug *ar) {
  int status;
  CallInfo *ci;
  if (level < 0) return 0;  /* invalid (negative) level */
  lua_lock(L);
  // 搜索指定层级的函数调用
  for (ci = L->ci; level > 0 && ci != &L->base_ci; ci = ci->previous)
    level--;

  // 是否找到对应的堆栈信息
  if (level == 0 && ci != &L->base_ci) {  /* level found? */
    status = 1;
    ar->i_ci = ci;
  }
  else status = 0;  /* no such level */
  lua_unlock(L);
  return status;
}

// 得到upvalue的名字
static const char *upvalname (Proto *p, int uv) {
  TString *s = check_exp(uv < p->sizeupvalues, p->upvalues[uv].name);
  if (s == NULL) return "?";
  else return getstr(s);
}

// 找到对应的可变参数
static const char *findvararg (CallInfo *ci, int n, StkId *pos) {
  int nparams = clLvalue(ci->func)->p->numparams;
  // 是否超出参数数量
  if (n >= cast_int(ci->u.l.base - ci->func) - nparams)
    return NULL;  /* no such vararg */
  else {
      // 找到可变参数
    *pos = ci->func + nparams + n;
    return "(*vararg)";  /* generic name for any vararg */
  }
}

// 查找局部变量的名字
static const char *findlocal (lua_State *L, CallInfo *ci, int n,
                              StkId *pos) {
  const char *name = NULL;
  StkId base;
  if (isLua(ci)) {
	  // 找参数的值
    if (n < 0)  /* access to vararg values? */
      return findvararg(ci, -n, pos);
    else {
      base = ci->u.l.base;
      // 返回局部变量的名字
      name = luaF_getlocalname(ci_func(ci)->p, n, currentpc(ci));
    }
  }
  else
    base = ci->func + 1;
  // 没有名字
  if (name == NULL) {  /* no 'standard' name? */
    // 得到变量的可能范围
    StkId limit = (ci == L->ci) ? L->top : ci->next->func;
    // 如果n在ci堆栈
    if (limit - base >= n && n > 0)  /* is 'n' inside 'ci' stack? */
      // 任何合法的slot的一般的名字
      name = "(*temporary)";  /* generic name for any valid slot */
    else
      return NULL;  /* no name */
  }
  // 位置
  *pos = base + (n - 1);
  return name;
}

// 得到变量
LUA_API const char *lua_getlocal (lua_State *L, const lua_Debug *ar, int n) {
  const char *name;
  lua_lock(L);
  swapextra(L);
  // 非活跃函数的信息
  if (ar == NULL) {  /* information about non-active function? */
    // 是否为lua函数
    if (!isLfunction(L->top - 1))  /* not a Lua function? */
      name = NULL;
    else  /* consider live variables at function start (parameters) */
       // 在函数开始时考虑实时变量（参数） 
      name = luaF_getlocalname(clLvalue(L->top - 1)->p, n, 0);
  }
  else {  /* active function; get information through 'ar' */
    // 实时函数，通过'ar'得到信息
    StkId pos = NULL;  /* to avoid warnings */
    // 找到变量的名字和位置
    name = findlocal(L, ar->i_ci, n, &pos);
    if (name) {
      setobj2s(L, L->top, pos);
      api_incr_top(L);
    }
  }
  swapextra(L);
  lua_unlock(L);
  return name;
}

// 设置局部变量
LUA_API const char *lua_setlocal (lua_State *L, const lua_Debug *ar, int n) {
  StkId pos = NULL;  /* to avoid warnings */
  const char *name;
  lua_lock(L);
  swapextra(L);
  // 搜索到局部变量
  name = findlocal(L, ar->i_ci, n, &pos);
  if (name) {
    // 将堆栈顶部的值赋给局部变量
    setobjs2s(L, pos, L->top - 1);
    // 出栈
    L->top--;  /* pop value */
  }
  swapextra(L);
  lua_unlock(L);
  return name;
}

// 函数信息
static void funcinfo (lua_Debug *ar, Closure *cl) {
    // c函数
  if (noLuaClosure(cl)) {
    ar->source = "=[C]";
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = "C";
  }
  else {
    Proto *p = cl->l.p;
    // 获取源码，行信息等
    ar->source = p->source ? getstr(p->source) : "=?";
    ar->linedefined = p->linedefined;
    ar->lastlinedefined = p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? "main" : "Lua";
  }
  // 从源码中拷贝前面的部分给简要源码用
  luaO_chunkid(ar->short_src, ar->source, LUA_IDSIZE);
}

// 
// 注意：Closure是一个C闭包和lua闭包的联合体
// 得到lua的代码行信息
static void collectvalidlines (lua_State *L, Closure *f) {
	// C闭包
  if (noLuaClosure(f)) {
    setnilvalue(L->top);
    api_incr_top(L);
  }
  else {
    int i;
    TValue v;
	// 得到代码行信息
    int *lineinfo = f->l.p->lineinfo;
    Table *t = luaH_new(L);  /* new table to store active lines */
    sethvalue(L, L->top, t);  /* push it on stack */
    api_incr_top(L);
	// 设置v为1
    setbvalue(&v, 1);  /* boolean 'true' to be the value of all indices */
	// 设置table[line] = true 
    for (i = 0; i < f->l.p->sizelineinfo; i++)  /* for all lines with code */
      luaH_setint(L, t, lineinfo[i], &v);  /* table[line] = true */
  }
}

// 得到函数的名字
static const char *getfuncname (lua_State *L, CallInfo *ci, const char **name) {
  if (ci == NULL)  /* no 'ci'? */
    return NULL;  /* no info */
  // 终结器
  else if (ci->callstatus & CIST_FIN) {  /* is this a finalizer? */
    *name = "__gc";
    return "metamethod";  /* report it as such */
  }
  // 不是尾调用，并且前一个是lua函数
  /* calling function is a known Lua function? */
  else if (!(ci->callstatus & CIST_TAIL) && isLua(ci->previous))
    return funcnamefromcode(L, ci->previous, name);
  else return NULL;  /* no way to find a name */
}

// 得到对应信息
static int auxgetinfo (lua_State *L, const char *what, lua_Debug *ar,
                       Closure *f, CallInfo *ci) {
  int status = 1;
  for (; *what; what++) {
    switch (*what) {
      case 'S': {
        funcinfo(ar, f);
        break;
      }
      case 'l': {
        ar->currentline = (ci && isLua(ci)) ? currentline(ci) : -1;
        break;
      }
      case 'u': {
        ar->nups = (f == NULL) ? 0 : f->c.nupvalues;
        if (noLuaClosure(f)) {
          ar->isvararg = 1;
          ar->nparams = 0;
        }
        else {
          ar->isvararg = f->l.p->is_vararg;
          ar->nparams = f->l.p->numparams;
        }
        break;
      }
	  // 是否是尾调用
      case 't': {
        ar->istailcall = (ci) ? ci->callstatus & CIST_TAIL : 0;
        break;
      }
      case 'n': {
        ar->namewhat = getfuncname(L, ci, &ar->name);
        if (ar->namewhat == NULL) {
          ar->namewhat = "";  /* not found */
          ar->name = NULL;
        }
        break;
      }
      case 'L':
      case 'f':  /* handled by lua_getinfo */
        break;
      default: status = 0;  /* invalid option */
    }
  }
  return status;
}


LUA_API int lua_getinfo (lua_State *L, const char *what, lua_Debug *ar) {
  int status;
  Closure *cl;
  CallInfo *ci;
  StkId func;
  lua_lock(L);
  swapextra(L);
  // 函数
  if (*what == '>') {
    ci = NULL;
	// 从栈顶上取得函数
    func = L->top - 1;
	// 检查是否是函数
    api_check(L, ttisfunction(func), "function expected");
    what++;  /* skip the '>' */
	// 函数出栈
    L->top--;  /* pop function */
  }
  else {
    ci = ar->i_ci;
    func = ci->func;
    lua_assert(ttisfunction(ci->func));
  }
  cl = ttisclosure(func) ? clvalue(func) : NULL;
  // 得到栈的信息
  status = auxgetinfo(L, what, ar, cl, ci);
  // 是否需要将函数返回
  if (strchr(what, 'f')) {
    setobjs2s(L, L->top, func);
    api_incr_top(L);
  }
  swapextra(L);  /* correct before option 'L', which can raise a mem. error */
  // 得到代码行信息
  if (strchr(what, 'L'))
    collectvalidlines(L, cl);
  lua_unlock(L);
  return status;
}


/*
** {======================================================
** Symbolic Execution
** =======================================================
*/
// 符号执行
static const char *getobjname (Proto *p, int lastpc, int reg,
                               const char **name);


/*
** find a "name" for the RK value 'c'
*/
// R：表示register， K：表示常量
// 为RK值 'c' 找到一个“名称” 
static void kname (Proto *p, int pc, int c, const char **name) {
  // c是否是一个常量
  if (ISK(c)) {  /* is 'c' a constant? */
    // 从常量表中得到信息
    TValue *kvalue = &p->k[INDEXK(c)];
    // 字面常量
    if (ttisstring(kvalue)) {  /* literal constant? */
      // 得到它自己的名字
      *name = svalue(kvalue);  /* it is its own name */
      return;
    }
    /* else no reasonable name found */
  }
  // c是一个寄存器
  else {  /* 'c' is a register */
      // 搜索'c'的信息
    const char *what = getobjname(p, pc, c, name); /* search for 'c' */
    // 如果得到一个常量，直接返回
    if (what && *what == 'c') {  /* found a constant name? */
      return;  /* 'name' already filled */
    }
    /* else no reasonable name found */
  }
  // 没有找到合理的名字
  *name = "?";  /* no reasonable name found */
}

// 过滤pc
static int filterpc (int pc, int jmptarget) {
   // 条件代码（在一个跳转里面）
  if (pc < jmptarget)  /* is code conditional (inside a jump)? */
    return -1;  /* cannot know who sets that register */
  else return pc;  /* current position sets that register */
}


/*
** try to find last instruction before 'lastpc' that modified register 'reg'
*/
// 尝试在修改寄存器“reg”的“lastpc”之前找到最后一条指令
static int findsetreg (Proto *p, int lastpc, int reg) {
  int pc;
  // 保存改变'reg'的上一条指令
  int setreg = -1;  /* keep last instruction that changed 'reg' */
  // 此地址之前的任何代码都是有条件的 
  int jmptarget = 0;  /* any code before this address is conditional */
  for (pc = 0; pc < lastpc; pc++) {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    int a = GETARG_A(i);
    switch (op) {
      case OP_LOADNIL: {
        int b = GETARG_B(i);
        if (a <= reg && reg <= a + b)  /* set registers from 'a' to 'a+b' */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_TFORCALL: {
        if (reg >= a + 2)  /* affect all regs above its base */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_CALL:
      case OP_TAILCALL: {
        if (reg >= a)  /* affect all registers above base */
          setreg = filterpc(pc, jmptarget);
        break;
      }
      case OP_JMP: {
        int b = GETARG_sBx(i);
        int dest = pc + 1 + b;
        /* jump is forward and do not skip 'lastpc'? */
        if (pc < dest && dest <= lastpc) {
          if (dest > jmptarget)
            jmptarget = dest;  /* update 'jmptarget' */
        }
        break;
      }
      default:
        if (testAMode(op) && reg == a)  /* any instruction that set A */
          setreg = filterpc(pc, jmptarget);
        break;
    }
  }
  return setreg;
}

// 得到实例的名字
static const char *getobjname (Proto *p, int lastpc, int reg,
                               const char **name) {
  int pc;
  // 从局部变量中找
  *name = luaF_getlocalname(p, reg + 1, lastpc);
  if (*name)  /* is a local? */
    return "local";

  // 尝试符号执行
  /* else try symbolic execution */
  pc = findsetreg(p, lastpc, reg);
  if (pc != -1) {  /* could find instruction? */
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    switch (op) {
      case OP_MOVE: {
        int b = GETARG_B(i);  /* move from 'b' to 'a' */
        if (b < GETARG_A(i))
          return getobjname(p, pc, b, name);  /* get name for 'b' */
        break;
      }
      case OP_GETTABUP:
      case OP_GETTABLE: {
        int k = GETARG_C(i);  /* key index */
        int t = GETARG_B(i);  /* table index */
        const char *vn = (op == OP_GETTABLE)  /* name of indexed variable */
                         ? luaF_getlocalname(p, t + 1, pc)
                         : upvalname(p, t);
        kname(p, pc, k, name);
        return (vn && strcmp(vn, LUA_ENV) == 0) ? "global" : "field";
      }
      case OP_GETUPVAL: {
        *name = upvalname(p, GETARG_B(i));
        return "upvalue";
      }
      case OP_LOADK:
      case OP_LOADKX: {
        int b = (op == OP_LOADK) ? GETARG_Bx(i)
                                 : GETARG_Ax(p->code[pc + 1]);
        if (ttisstring(&p->k[b])) {
          *name = svalue(&p->k[b]);
          return "constant";
        }
        break;
      }
      case OP_SELF: {
        int k = GETARG_C(i);  /* key index */
        kname(p, pc, k, name);
        return "method";
      }
      default: break;  /* go through to return NULL */
    }
  }
  return NULL;  /* could not find reasonable name */
}


/*
** Try to find a name for a function based on the code that called it.
** (Only works when function was called by a Lua function.)
** Returns what the name is (e.g., "for iterator", "method",
** "metamethod") and sets '*name' to point to the name.
*/
// 尝试根据调用函数的代码查找函数的名称。
// （仅当函数被lua函数调用时有作用）
// 
static const char *funcnamefromcode (lua_State *L, CallInfo *ci,
                                     const char **name) {
  TMS tm = (TMS)0;  /* (initial value avoids warnings) */
  // 取出函数原型
  Proto *p = ci_func(ci)->p;  /* calling function */
  // 得到当前指令到函数开始处的距离
  int pc = currentpc(ci);  /* calling instruction index */
  // 得到当前的指令
  Instruction i = p->code[pc];  /* calling instruction */
  // 调试钩子
  if (ci->callstatus & CIST_HOOKED) {  /* was it called inside a hook? */
    *name = "?";
    return "hook";
  }
  // 得到操作码
  switch (GET_OPCODE(i)) {
    case OP_CALL:
    case OP_TAILCALL:
      return getobjname(p, pc, GETARG_A(i), name);  /* get function name */
	  // 迭代器
    case OP_TFORCALL: {  /* for iterator */
      *name = "for iterator";
       return "for iterator";
    }
    /* other instructions can do calls through metamethods */
	// 其他的指令调用通过元方法
    case OP_SELF: case OP_GETTABUP: case OP_GETTABLE:
      tm = TM_INDEX;
      break;
    case OP_SETTABUP: case OP_SETTABLE:
      tm = TM_NEWINDEX;
      break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
    case OP_POW: case OP_DIV: case OP_IDIV: case OP_BAND:
    case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
      int offset = cast_int(GET_OPCODE(i)) - cast_int(OP_ADD);  /* ORDER OP */
      tm = cast(TMS, offset + cast_int(TM_ADD));  /* ORDER TM */
      break;
    }
    case OP_UNM: tm = TM_UNM; break;
    case OP_BNOT: tm = TM_BNOT; break;
    case OP_LEN: tm = TM_LEN; break;
    case OP_CONCAT: tm = TM_CONCAT; break;
    case OP_EQ: tm = TM_EQ; break;
    case OP_LT: tm = TM_LT; break;
    case OP_LE: tm = TM_LE; break;
    default:
      return NULL;  /* cannot find a reasonable name */
  }
  *name = getstr(G(L)->tmname[tm]);
  return "metamethod";
}

/* }====================================================== */



/*
** The subtraction of two potentially unrelated pointers is
** not ISO C, but it should not crash a program; the subsequent
** checks are ISO C and ensure a correct result.
*/
// 两个可能不相关的指针相减不是ISO C，但它不应该使程序崩溃；随后的检查时ISO C并且确保一个正确的结果
// 是否在堆栈里
static int isinstack (CallInfo *ci, const TValue *o) {
  // 计算栈底的偏移
  ptrdiff_t i = o - ci->u.l.base;
  // 是否在当前堆栈的范围内
  return (0 <= i && i < (ci->top - ci->u.l.base) && ci->u.l.base + i == o);
}


/*
** Checks whether value 'o' came from an upvalue. (That can only happen
** with instructions OP_GETTABUP/OP_SETTABUP, which operate directly on
** upvalues.)
*/ 
// 检验是否值'o'来自一个upvalue。
//（这只能通过指令 P_GETTABUP/OP_SETTABUP 发生，它们直接对 upvalue 进行操作。）
static const char *getupvalname (CallInfo *ci, const TValue *o,
                                 const char **name) {
  // 只有lua函数有upvalue
  LClosure *c = ci_func(ci);
  int i;
  // 遍历调用函数的upvalue
  for (i = 0; i < c->nupvalues; i++) {
      // 找到upvalues，得到对应的名字
    if (c->upvals[i]->v == o) {
      *name = upvalname(c->p, i);
      return "upvalue";
    }
  }
  return NULL;
}

// 得到一个变量的信息
static const char *varinfo (lua_State *L, const TValue *o) {
  const char *name = NULL;  /* to avoid warnings */
  CallInfo *ci = L->ci;
  const char *kind = NULL;
  // 是lua函数
  if (isLua(ci)) {
      // 是否是upvalues，如果是就得到upvalue的名字
    kind = getupvalname(ci, o, &name);  /* check whether 'o' is an upvalue */
    // 不是upvalues，就看看是否是寄存器
    if (!kind && isinstack(ci, o))  /* no? try a register */
      kind = getobjname(ci_func(ci)->p, currentpc(ci),
                        cast_int(o - ci->u.l.base), &name);
  }
  // 返回变量的类型和名字
  return (kind) ? luaO_pushfstring(L, " (%s '%s')", kind, name) : "";
}


// 操作时出现类型错误
l_noret luaG_typeerror (lua_State *L, const TValue *o, const char *op) {
    // 得到类型和名字
  const char *t = luaT_objtypename(L, o);
  luaG_runerror(L, "attempt to %s a %s value%s", op, t, varinfo(L, o));
}

// 连接时出现类型错误
l_noret luaG_concaterror (lua_State *L, const TValue *p1, const TValue *p2) {
    // 如果p1有问题，先报p1的问题，如果p1没有问题，就报p2的问题
  if (ttisstring(p1) || cvt2str(p1)) p1 = p2;
  luaG_typeerror(L, p1, "concatenate");
}

// 整型操作错误
l_noret luaG_opinterror (lua_State *L, const TValue *p1,
                         const TValue *p2, const char *msg) {
  lua_Number temp;
  // 判断第一个操作数错误 
  if (!tonumber(p1, &temp))  /* first operand is wrong? */
      // 那就是第二个操作数错误
    p2 = p1;  /* now second is wrong */
  luaG_typeerror(L, p2, msg);
}


/*
** Error when both values are convertible to numbers, but not to integers
*/
// 当两个值都可以转换为数字但不能转换为整数时出错 
l_noret luaG_tointerror (lua_State *L, const TValue *p1, const TValue *p2) {
  lua_Integer temp;
  if (!tointeger(p1, &temp))
    p2 = p1;
  luaG_runerror(L, "number%s has no integer representation", varinfo(L, p2));
}

// 排序错误，
l_noret luaG_ordererror (lua_State *L, const TValue *p1, const TValue *p2) {
  const char *t1 = luaT_objtypename(L, p1);
  const char *t2 = luaT_objtypename(L, p2);
  // 两个类型相同的比较出错，还是不同类型的比较出错
  if (strcmp(t1, t2) == 0)
    luaG_runerror(L, "attempt to compare two %s values", t1);
  else
    luaG_runerror(L, "attempt to compare %s with %s", t1, t2);
}


/* add src:line information to 'msg' */
// 给msg增加src:line的信息
const char *luaG_addinfo (lua_State *L, const char *msg, TString *src,
                                        int line) {
  char buff[LUA_IDSIZE];
  if (src)
    luaO_chunkid(buff, getstr(src), LUA_IDSIZE);
  else {  /* no source available; use "?" instead */
    buff[0] = '?'; buff[1] = '\0';
  }
  return luaO_pushfstring(L, "%s:%d: %s", buff, line, msg);
}

// 错误信息
l_noret luaG_errormsg (lua_State *L) {
    // 是否有错误处理函数
  if (L->errfunc != 0) {  /* is there an error handling function? */
    StkId errfunc = restorestack(L, L->errfunc);
    // 将参数移动到栈顶
    setobjs2s(L, L->top, L->top - 1);  /* move argument */
    // 压入函数
    setobjs2s(L, L->top - 1, errfunc);  /* push function */
    L->top++;  /* assume EXTRA_STACK */
    // 调用处理函数
    luaD_callnoyield(L, L->top - 2, 1);  /* call it */
  }
  // 抛出运行出错
  luaD_throw(L, LUA_ERRRUN);
}

// 运行时错误
l_noret luaG_runerror (lua_State *L, const char *fmt, ...) {
  CallInfo *ci = L->ci;
  const char *msg;
  va_list argp;
  luaC_checkGC(L);  /* error message uses memory */
  // 格式化错误信息
  va_start(argp, fmt);
  msg = luaO_pushvfstring(L, fmt, argp);  /* format message */
  va_end(argp);
  // 如果是lua信息
  if (isLua(ci))  /* if Lua function, add source:line information */
    luaG_addinfo(L, msg, ci_func(ci)->p->source, currentline(ci));
  luaG_errormsg(L);
}


void luaG_traceexec (lua_State *L) {
  CallInfo *ci = L->ci;
  lu_byte mask = L->hookmask;
  // 如果hookcount计算为0，并且是LUA_MASKCOUNT
  int counthook = (--L->hookcount == 0 && (mask & LUA_MASKCOUNT));
  // 重置计数
  if (counthook)
    resethookcount(L);  /* reset count */
  // 不是LUA_MASKLINE的钩子
  else if (!(mask & LUA_MASKLINE))
    return;  /* no line hook and count != 0; nothing to be done */
  // 调用钩子状态，就将状态移除
  if (ci->callstatus & CIST_HOOKYIELD) {  /* called hook last time? */
    ci->callstatus &= ~CIST_HOOKYIELD;  /* erase mark */
    return;  /* do not call hook again (VM yielded, so it did not move) */
  }
  // 调用计数钩子
  if (counthook)
    luaD_hook(L, LUA_HOOKCOUNT, -1);  /* call count hook */
  // 行
  if (mask & LUA_MASKLINE) {
    Proto *p = ci_func(ci)->p;
    int npc = pcRel(ci->u.l.savedpc, p);
    int newline = getfuncline(p, npc);
    // 当进入一个新的函数，调用linehook
    if (npc == 0 ||  /* call linehook when enter a new function, */
        ci->u.l.savedpc <= L->oldpc ||  /* when jump back (loop), or when */
        newline != getfuncline(p, pcRel(L->oldpc, p)))  /* enter a new line */
        // 调用line钩子
      luaD_hook(L, LUA_HOOKLINE, newline);  /* call line hook */
  }
  L->oldpc = ci->u.l.savedpc;
  // 钩子yield
  if (L->status == LUA_YIELD) {  /* did hook yield? */
      // 恢复计数
    if (counthook)
      L->hookcount = 1;  /* undo decrement to zero */
    // 恢复pc计数
    ci->u.l.savedpc--;  /* undo increment (resume will increment it again) */
    // 标记yield
    ci->callstatus |= CIST_HOOKYIELD;  /* mark that it yielded */
    ci->func = L->top - 1;  /* protect stack below results */
    luaD_throw(L, LUA_YIELD);
  }
}

