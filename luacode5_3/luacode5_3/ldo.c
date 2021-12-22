/*
** $Id: ldo.c,v 2.157.1.1 2017/04/19 17:20:42 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#define ldo_c
#define LUA_CORE

#include "lprefix.h"


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"



#define errorstatus(s)	((s) > LUA_YIELD)


/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/*
** LUAI_THROW/LUAI_TRY define how Lua does exception handling. By
** default, Lua handles errors with exceptions when compiling as
** C++ code, with _longjmp/_setjmp when asked to use them, and with
** longjmp/setjmp otherwise.
*/
#if !defined(LUAI_THROW)				/* { */

#if defined(__cplusplus) && !defined(LUA_USE_LONGJMP)	/* { */

/* C++ exceptions */
#define LUAI_THROW(L,c)		throw(c)
#define LUAI_TRY(L,c,a) \
	try { a } catch(...) { if ((c)->status == 0) (c)->status = -1; }
#define luai_jmpbuf		int  /* dummy variable */

#elif defined(LUA_USE_POSIX)				/* }{ */

/* in POSIX, try _longjmp/_setjmp (more efficient) */
#define LUAI_THROW(L,c)		_longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (_setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#else							/* }{ */

/* ISO C handling with long jumps */
#define LUAI_THROW(L,c)		longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#endif							/* } */

#endif							/* } */



/* chain list of long jump buffers */
// 长跳转Buffer的链表
struct lua_longjmp {
  // 链表
  struct lua_longjmp *previous;
  // 跳转Buf
  luai_jmpbuf b;
  // 错误码
  volatile int status;  /* error code */
};

// 设置错误对象
static void seterrorobj (lua_State *L, int errcode, StkId oldtop) {
  switch (errcode) {
    // 内存错误
    case LUA_ERRMEM: {  /* memory error? */
      setsvalue2s(L, oldtop, G(L)->memerrmsg); /* reuse preregistered msg. */
      break;
    }
    // Lua错误
    case LUA_ERRERR: {
      setsvalue2s(L, oldtop, luaS_newliteral(L, "error in error handling"));
      break;
    }
    default: {
      // 错误信息在栈顶
      setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
      break;
    }
  }
  L->top = oldtop + 1;
}


l_noret luaD_throw (lua_State *L, int errcode) {
    // 线程有错误跳转
  if (L->errorJmp) {  /* thread has an error handler? */
    // 设置错误码
    L->errorJmp->status = errcode;  /* set status */
    // 跳转
    LUAI_THROW(L, L->errorJmp);  /* jump to it */
  }
  // 线程没有错误处理
  else {  /* thread has no error handler */
    global_State *g = G(L);
    // 标记为死线程
    L->status = cast_byte(errcode);  /* mark it as dead */
    // 主线程有错误处理
    if (g->mainthread->errorJmp) {  /* main thread has a handler? */
        // 错误对象拷贝到主线程
      setobjs2s(L, g->mainthread->top++, L->top - 1);  /* copy error obj. */
      // 重新抛出错误
      luaD_throw(g->mainthread, errcode);  /* re-throw in main thread */
    }
    // 没有错误处理
    else {  /* no handler at all; abort */
      // 最后的错误处理函数
      if (g->panic) {  /* panic function? */
        seterrorobj(L, errcode, L->top);  /* assume EXTRA_STACK */
        if (L->ci->top < L->top)
          L->ci->top = L->top;  /* pushing msg. can break this invariant */
        lua_unlock(L);
        // 调用错误处理函数
        g->panic(L);  /* call panic function (last chance to jump out) */
      }
      abort();
    }
  }
}


int luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud) {
    // 记录嵌套 C 调用的数量
  unsigned short oldnCcalls = L->nCcalls;

  // 准备异常处理
  struct lua_longjmp lj;
  lj.status = LUA_OK;
  // 链接新的错误分发
  lj.previous = L->errorJmp;  /* chain new error handler */
  L->errorJmp = &lj;
  // 异常处理中执行
  LUAI_TRY(L, &lj,
    (*f)(L, ud);
  );
  // 回复错误处理和C调用嵌套的数量
  L->errorJmp = lj.previous;  /* restore old error handler */
  L->nCcalls = oldnCcalls;
  return lj.status;
}

/* }====================================================== */


/*
** {==================================================================
** Stack reallocation
** ===================================================================
*/
// 堆栈重新分配以后，upvalu需要重新定位，调用信息需要重新定位
static void correctstack (lua_State *L, TValue *oldstack) {
  CallInfo *ci;
  UpVal *up;
  L->top = (L->top - oldstack) + L->stack;
  // upvalue重新定位
  for (up = L->openupval; up != NULL; up = up->u.open.next)
    up->v = (up->v - oldstack) + L->stack;
  // 调用信息重新定位
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    ci->top = (ci->top - oldstack) + L->stack;
    ci->func = (ci->func - oldstack) + L->stack;
    if (isLua(ci))
      ci->u.l.base = (ci->u.l.base - oldstack) + L->stack;
  }
}


/* some space for error handling */
#define ERRORSTACKSIZE	(LUAI_MAXSTACK + 200)

// 重新分配堆栈
void luaD_reallocstack (lua_State *L, int newsize) {
  TValue *oldstack = L->stack;
  int lim = L->stacksize;
  lua_assert(newsize <= LUAI_MAXSTACK || newsize == ERRORSTACKSIZE);
  lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
  luaM_reallocvector(L, L->stack, L->stacksize, newsize, TValue);
  // 新创建出来的值置为空值
  for (; lim < newsize; lim++)
    setnilvalue(L->stack + lim); /* erase new segment */
  // 设置新的尺寸
  L->stacksize = newsize;
  L->stack_last = L->stack + newsize - EXTRA_STACK;
  correctstack(L, oldstack);
}


void luaD_growstack (lua_State *L, int n) {
  int size = L->stacksize;
  // 是否已经大于最大值了
  if (size > LUAI_MAXSTACK)  /* error after extra size? */
    luaD_throw(L, LUA_ERRERR);
  else {
	  // 需要的大小
    int needed = cast_int(L->top - L->stack) + n + EXTRA_STACK;
	// 目前的大小扩大2倍
    int newsize = 2 * size;
	// 大于最大值就强制置成最大值
    if (newsize > LUAI_MAXSTACK) newsize = LUAI_MAXSTACK;
	// 新的大小是否符合需求
    if (newsize < needed) newsize = needed;
    if (newsize > LUAI_MAXSTACK) {  /* stack overflow? */
      luaD_reallocstack(L, ERRORSTACKSIZE);
      luaG_runerror(L, "stack overflow");
    }
    else
		// 重新分配
      luaD_reallocstack(L, newsize);
  }
}

// 遍历整个调用列表，找到最大值
static int stackinuse (lua_State *L) {
  CallInfo *ci;
  StkId lim = L->top;
  // 找到最大值
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    if (lim < ci->top) lim = ci->top;
  }
  lua_assert(lim <= L->stack_last);
  return cast_int(lim - L->stack) + 1;  /* part of stack in use */
}

// 栈收缩
void luaD_shrinkstack (lua_State *L) {
  // 在用的栈大小
  int inuse = stackinuse(L);
  // 
  int goodsize = inuse + (inuse / 8) + 2*EXTRA_STACK;
  // 如果超过最大的大小，强制使用最大大小
  if (goodsize > LUAI_MAXSTACK)
    goodsize = LUAI_MAXSTACK;  /* respect stack limit */
  // 如果栈大小已经超过上限，就释放所有的调用信息
  if (L->stacksize > LUAI_MAXSTACK)  /* had been handling stack overflow? */
    luaE_freeCI(L);  /* free all CIs (list grew because of an error) */
  else
    // 收缩调用信息
    luaE_shrinkCI(L);  /* shrink list */
  /* if thread is currently not handling a stack overflow and its
     good size is smaller than current size, shrink its stack */
  // 如果线程当前没有堆栈溢出及其适合尺寸小于当前尺寸，缩小其堆栈
  if (inuse <= (LUAI_MAXSTACK - EXTRA_STACK) &&
      goodsize < L->stacksize)
    luaD_reallocstack(L, goodsize);
  else  /* don't change stack */
    condmovestack(L,{},{});  /* (change only for debugging) */
}

// 自增长堆栈
void luaD_inctop (lua_State *L) {
  luaD_checkstack(L, 1);
  L->top++;
}

/* }================================================================== */


/*
** Call a hook for the given event. Make sure there is a hook to be
** called. (Both 'L->hook' and 'L->hookmask', which triggers this
** function, can be changed asynchronously by signals.)
*/
// 为给定的事件调用一个钩子。 确保有一个钩子被调用。
// L->hook和L->hookmask都触发了该函数，可以通过信号异步更改
void luaD_hook (lua_State *L, int event, int line) {
  lua_Hook hook = L->hook;
  // 确定有一个钩子
  if (hook && L->allowhook) {  /* make sure there is a hook */
    CallInfo *ci = L->ci;
    // 保存全局栈顶和调用栈顶
    ptrdiff_t top = savestack(L, L->top);
    ptrdiff_t ci_top = savestack(L, ci->top);

    lua_Debug ar;
    ar.event = event;
    ar.currentline = line;
    ar.i_ci = ci;
    // 确定最小的栈大小
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
    ci->top = L->top + LUA_MINSTACK;
    lua_assert(ci->top <= L->stack_last);
    // 不再允许调用钩子函数
    L->allowhook = 0;  /* cannot call hooks inside a hook */
    // 将调用状态设置为调用钩子状态
    ci->callstatus |= CIST_HOOKED;
    lua_unlock(L);
    // 调用钩子函数
    (*hook)(L, &ar);
    lua_lock(L);
    lua_assert(!L->allowhook);
    // 恢复以前的状态
    L->allowhook = 1;
    ci->top = restorestack(L, ci_top);
    L->top = restorestack(L, top);
    ci->callstatus &= ~CIST_HOOKED;
  }
}

// 调用钩子
static void callhook (lua_State *L, CallInfo *ci) {
  int hook = LUA_HOOKCALL;
  // 钩子假定pc已经增加了
  ci->u.l.savedpc++;  /* hooks assume 'pc' is already incremented */
  // 上一个调用时lua并且是尾调用
  if (isLua(ci->previous) &&
      GET_OPCODE(*(ci->previous->u.l.savedpc - 1)) == OP_TAILCALL) {
    ci->callstatus |= CIST_TAIL;
    hook = LUA_HOOKTAILCALL;
  }
  // 调用钩子
  luaD_hook(L, hook, -1);
  // 矫正pc
  ci->u.l.savedpc--;  /* correct 'pc' */
}


// 调整参数，nfixargs需求的，actual实际上的
// 为啥要移动一次呢？
static StkId adjust_varargs (lua_State *L, Proto *p, int actual) {
  int i;
  int nfixargs = p->numparams;
  StkId base, fixed;
  /* move fixed parameters to final position */
  fixed = L->top - actual;  /* first fixed argument */
  base = L->top;  /* final position of first argument */
  // 把函数的参数从上一个堆栈的空间拷贝过来
  for (i = 0; i < nfixargs && i < actual; i++) {
    setobjs2s(L, L->top++, fixed + i);
	// 删除原来的
    setnilvalue(fixed + i);  /* erase original copy (for GC) */
  }
  // 如果实际上没有传入想要的那么多参数，那剩下的置空
  for (; i < nfixargs; i++)
    setnilvalue(L->top++);  /* complete missing arguments */
  return base;
}


/*
** Check whether __call metafield of 'func' is a function. If so, put
** it in stack below original 'func' so that 'luaD_precall' can call
** it. Raise an error if __call metafield is not a function.
*/
// 尝试TM_CALL的元方法
static void tryfuncTM (lua_State *L, StkId func) {
  const TValue *tm = luaT_gettmbyobj(L, func, TM_CALL);
  StkId p;
  if (!ttisfunction(tm))
    luaG_typeerror(L, func, "call");
  /* Open a hole inside the stack at 'func' */
  // 后移一个（包括原来的函数 )，空出一个位置来放置元方法
  for (p = L->top; p > func; p--)
    setobjs2s(L, p, p-1);
  L->top++;  /* slot ensured by caller */
  setobj2s(L, func, tm);  /* tag method is the new function to be called */
}


/*
** Given 'nres' results at 'firstResult', move 'wanted' of them to 'res'.
** Handle most typical cases (zero results for commands, one result for
** expressions, multiple results for tail calls/single parameters)
** separated.
*/
// 在'firstResult'中给定'nres'个结果,移动它们中的'wanted'个到'res'.
// 最典型的处理方法（命令无结果，表达式一个结果，尾调用或者单参数的多个结果）
// moveresults会将结果集（0个/1个/多个），逐个根据顺序拷贝到ci->func位置，并调整L->top的指针位置，如果返回值不够，会用nil填充
// nres：实际的返回值数目
// wanted：要求的返回值数目
static int moveresults (lua_State *L, const TValue *firstResult, StkId res,
                                      int nres, int wanted) {
  switch (wanted) {  /* handle typical cases separately */
	  // 不需要返回值，直接不处理
    case 0: break;  /* nothing to move */
		// 只需要一个返回值，
    case 1: {  /* one result needed */
		// 如果没有返回值，就放回nil
      if (nres == 0)   /* no results? */
        firstResult = luaO_nilobject;  /* adjust with nil */
      setobjs2s(L, res, firstResult);  /* move it to proper place */
      break;
    }
    case LUA_MULTRET: {
      int i;
	  // 所有的都返回
      for (i = 0; i < nres; i++)  /* move all results to correct place */
        setobjs2s(L, res + i, firstResult + i);
      L->top = res + nres;
      return 0;  /* wanted == LUA_MULTRET */
    }
    default: {
      int i;
	  // 移到合适的，如果不够，置空
      if (wanted <= nres) {  /* enough results? */
        for (i = 0; i < wanted; i++)  /* move wanted results to correct place */
          setobjs2s(L, res + i, firstResult + i);
      }
      else {  /* not enough results; use all of them plus nils */
        for (i = 0; i < nres; i++)  /* move all results to correct place */
          setobjs2s(L, res + i, firstResult + i);
        for (; i < wanted; i++)  /* complete wanted number of results */
          setnilvalue(res + i);
      }
      break;
    }
  }
  L->top = res + wanted;  /* top points after the last result */
  return 1;
}


/*
** Finishes a function call: calls hook if necessary, removes CallInfo,
** moves current number of results to proper place; returns 0 iff call
** wanted multiple (variable number of) results.
*/
// 完成一个函数调用：必要时调用hook，删除CallInfo，将返回值移到合适的位置
int luaD_poscall (lua_State *L, CallInfo *ci, StkId firstResult, int nres) {
  StkId res;
  int wanted = ci->nresults;
  if (L->hookmask & (LUA_MASKRET | LUA_MASKLINE)) {
    if (L->hookmask & LUA_MASKRET) {
		// 由于hook可能改变堆栈，可以调用前需要保存，调用后恢复
      ptrdiff_t fr = savestack(L, firstResult);  /* hook may change stack */
      luaD_hook(L, LUA_HOOKRET, -1);
      firstResult = restorestack(L, fr);
    }
    L->oldpc = ci->previous->u.l.savedpc;  /* 'oldpc' for caller function */
  }
  // 
  res = ci->func;  /* res == final position of 1st result */
  // 回退到调用者
  L->ci = ci->previous;  /* back to caller */
  /* move results to proper place */
  // 将结果移动到合适的位置
  return moveresults(L, firstResult, res, nres, wanted);
}


// 重用callInfo，如果没有重用的，就创建一个新的
#define next_ci(L) (L->ci = (L->ci->next ? L->ci->next : luaE_extendCI(L)))


/* macro to check stack size, preserving 'p' */
// 保证至少有n大小的堆栈大小
#define checkstackp(L,n,p)  \
  luaD_checkstackaux(L, n, \
    ptrdiff_t t__ = savestack(L, p);  /* save 'p' */ \
    luaC_checkGC(L),  /* stack grow uses memory */ \
    p = restorestack(L, t__))  /* 'pos' part: restore 'p' */


/*
** Prepares a function call: checks the stack, creates a new CallInfo
** entry, fills in the relevant information, calls hook if needed.
** If function is a C function, does the call, too. (Otherwise, leave
** the execution ('luaV_execute') to the caller, to allow stackless
** calls.) Returns true iff function has been executed (C function).
*/
// 准备一个函数调用
int luaD_precall (lua_State *L, StkId func, int nresults) {
  lua_CFunction f;
  CallInfo *ci;
  // 根据函数的类型调用：C闭包，轻量级C函数，Lua函数
  switch (ttype(func)) {
      // C闭包和轻量C函数执行取值的方式不一样
      // C闭包
    case LUA_TCCL:  /* C closure */
      f = clCvalue(func)->f;
      goto Cfunc;
    // 轻量C函数，
    case LUA_TLCF:  /* light C function */
      f = fvalue(func);
     Cfunc: {
      int n;  /* number of returns */
	  // 保证最小的堆栈大小
      checkstackp(L, LUA_MINSTACK, func);  /* ensure minimum stack size */

	  // 进入新的函数（调用信息callInfo)
      // 重用或者创建新的CallInfo
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;
      ci->func = func;
      // 直接增加LUA_MINSTACK堆栈
      ci->top = L->top + LUA_MINSTACK;
      lua_assert(ci->top <= L->stack_last);
      ci->callstatus = 0;
      if (L->hookmask & LUA_MASKCALL)
        luaD_hook(L, LUA_HOOKCALL, -1);
      lua_unlock(L);
	  // 真正的调用，n表示返回值的数目
      n = (*f)(L);  /* do the actual call */
      lua_lock(L);
      api_checknelems(L, n);
	  // 调用后的处理
      luaD_poscall(L, ci, L->top - n, n);
      return 1;
    }
    case LUA_TLCL: {  /* Lua function: prepare its call */
      StkId base;
      Proto *p = clLvalue(func)->p;
	  // 计算出参数的数目
      // func后面就是参数
      int n = cast_int(L->top - func) - 1;  /* number of real arguments */
      // 此函数所需的寄存器数量为最大的堆栈尺寸
      int fsize = p->maxstacksize;  /* frame size */
      checkstackp(L, fsize, func);
      // base总是指向第一个固定参数处
      // 可变参数：func + 空洞（）+ 可变参数+固定参数，这里的空洞是移走的固定参数
      if (p->is_vararg)
        base = adjust_varargs(L, p, n);
      // 固定参数：func + 固定参数
      else {  /* non vararg function */
		  // 将不够的参数补齐，都是nil
        for (; n < p->numparams; n++)
          setnilvalue(L->top++);  /* complete missing arguments */
        // 函数地址后面就是新的CallInfo
        base = func + 1;
      }

	  // 进入新的函数（调用信息callInfo)
      // 重用或者创建新的CallInfo
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;
      ci->func = func;
      ci->u.l.base = base;
      L->top = ci->top = base + fsize;
      lua_assert(ci->top <= L->stack_last);
	  // 保存开始调用点
      ci->u.l.savedpc = p->code;  /* starting point */
      ci->callstatus = CIST_LUA;
      if (L->hookmask & LUA_MASKCALL)
        callhook(L, ci);
	  // 注意Lua函数在这里并不真正调用
      return 0;
    }
    default: {  /* not a function */
      checkstackp(L, 1, func);  /* ensure space for metamethod */
      tryfuncTM(L, func);  /* try to get '__call' metamethod */
      return luaD_precall(L, func, nresults);  /* now it must be a function */
    }
  }
}


/*
** Check appropriate error for stack overflow ("regular" overflow or
** overflow while handling stack overflow). If 'nCalls' is larger than
** LUAI_MAXCCALLS (which means it is handling a "regular" overflow) but
** smaller than 9/8 of LUAI_MAXCCALLS, does not report an error (to
** allow overflow handling to work)
*/
// 检查堆栈溢出的适当错误（“常规”溢出或处理堆栈溢出时溢出）。 如果“nCalls”大于
// LUAI_MAXCCALLS（这意味着它正在处理“常规”溢出）但小于 LUAI_MAXCCALLS 的 9 / 8，不报告错误（以允许溢出处理工作）
static void stackerror (lua_State *L) {
  if (L->nCcalls == LUAI_MAXCCALLS)
    luaG_runerror(L, "C stack overflow");
  else if (L->nCcalls >= (LUAI_MAXCCALLS + (LUAI_MAXCCALLS>>3)))
    luaD_throw(L, LUA_ERRERR);  /* error while handing stack error */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/
void luaD_call (lua_State *L, StkId func, int nResults) {
	// 调用不能大于最大调用层数
  if (++L->nCcalls >= LUAI_MAXCCALLS)
    stackerror(L);
  // 只有lua函数才需要调用luaV_execute
  if (!luaD_precall(L, func, nResults))  /* is a Lua function? */
    luaV_execute(L);  /* call it */
  L->nCcalls--;
}


/*
** Similar to 'luaD_call', but does not allow yields during the call
*/
// 和luaD_call很相似，但是在调用期间不允许yield
void luaD_callnoyield (lua_State *L, StkId func, int nResults) {
  L->nny++;
  luaD_call(L, func, nResults);
  L->nny--;
}


/*
** Completes the execution of an interrupted C function, calling its
** continuation function.
*/
// 完成一个被中断的C函数的执行，调用它的后续函数。
static void finishCcall (lua_State *L, int status) {
  CallInfo *ci = L->ci;
  int n;
  /* must have a continuation and must be able to call it */
  // 保证有一个后续函数并且能够调用它
  lua_assert(ci->u.c.k != NULL && L->nny == 0);
  /* error status can only happen in a protected call */
  // 错误状态只发生在受保护的调用中
  lua_assert((ci->callstatus & CIST_YPCALL) || status == LUA_YIELD);
  // 在pcall里面
  if (ci->callstatus & CIST_YPCALL) {  /* was inside a pcall? */
    ci->callstatus &= ~CIST_YPCALL;  /* continuation is also inside it */
    L->errfunc = ci->u.c.old_errfunc;  /* with the same error function */
  }
  /* finish 'lua_callk'/'lua_pcall'; CIST_YPCALL and 'errfunc' already
     handled */
  // 完成'lua_callk'/'lua_pcall';CIST_YPCALL和'errfunc'已经处理
  adjustresults(L, ci->nresults);
  lua_unlock(L);
  // 调用后续函数
  n = (*ci->u.c.k)(L, status, ci->u.c.ctx);  /* call continuation function */
  lua_lock(L);
  api_checknelems(L, n);
  // 完成luaD_precall调用
  luaD_poscall(L, ci, L->top - n, n);  /* finish 'luaD_precall' */
}


/*
** Executes "full continuation" (everything in the stack) of a
** previously interrupted coroutine until the stack is empty (or another
** interruption long-jumps out of the loop). If the coroutine is
** recovering from an error, 'ud' points to the error status, which must
** be passed to the first continuation function (otherwise the default
** status is LUA_YIELD).
*/
static void unroll (lua_State *L, void *ud) {
  if (ud != NULL)  /* error status? */
    finishCcall(L, *(int *)ud);  /* finish 'lua_pcallk' callee */
  while (L->ci != &L->base_ci) {  /* something in the stack */
    if (!isLua(L->ci))  /* C function? */
      finishCcall(L, LUA_YIELD);  /* complete its execution */
    else {  /* Lua function */
      luaV_finishOp(L);  /* finish interrupted instruction */
      luaV_execute(L);  /* execute down to higher C 'boundary' */
    }
  }
}


/*
** Try to find a suspended protected call (a "recover point") for the
** given thread.
*/
// 尝试找到给定线程挂起的受保护调用("恢复点")
static CallInfo *findpcall (lua_State *L) {
  CallInfo *ci;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {  /* search for a pcall */
    if (ci->callstatus & CIST_YPCALL)
      return ci;
  }
  return NULL;  /* no pending pcall */
}


/*
** Recovers from an error in a coroutine. Finds a recover point (if
** there is one) and completes the execution of the interrupted
** 'luaD_pcall'. If there is no recover point, returns zero.
*/
// 从一个协程的错误中恢复。如果有的话，找到一个恢复点并且完成被中断的luaD_pcall，如果没有恢复点，就返回0
static int recover (lua_State *L, int status) {
  StkId oldtop;
  // 找到调用信息
  CallInfo *ci = findpcall(L);
  if (ci == NULL) return 0;  /* no recovery point */
  /* "finish" luaD_pcall */
  // 恢复堆栈
  oldtop = restorestack(L, ci->extra);
  luaF_close(L, oldtop);
  seterrorobj(L, status, oldtop);
  L->ci = ci;
  L->allowhook = getoah(ci->callstatus);  /* restore original 'allowhook' */
  // 可以yield
  L->nny = 0;  /* should be zero to be yieldable */
  // 缩小堆栈
  luaD_shrinkstack(L);
  L->errfunc = ci->u.c.old_errfunc;
  return 1;  /* continue running the coroutine */
}


/*
** Signal an error in the call to 'lua_resume', not in the execution
** of the coroutine itself. (Such errors should not be handled by any
** coroutine error handler and should not kill the coroutine.)
*/
static int resume_error (lua_State *L, const char *msg, int narg) {
  L->top -= narg;  /* remove args from the stack */
  setsvalue2s(L, L->top, luaS_new(L, msg));  /* push error message */
  api_incr_top(L);
  lua_unlock(L);
  return LUA_ERRRUN;
}


/*
** Do the work for 'lua_resume' in protected mode. Most of the work
** depends on the status of the coroutine: initial state, suspended
** inside a hook, or regularly suspended (optionally with a continuation
** function), plus erroneous cases: non-suspended coroutine or dead
** coroutine.
*/
// 
static void resume (lua_State *L, void *ud) {
  int n = *(cast(int*, ud));  /* number of arguments */
  StkId firstArg = L->top - n;  /* first argument */
  CallInfo *ci = L->ci;
  // 如果当前协程的状态为LUA_OK，表示第一次执行resume操作，此时调用luaD_precall做函数调用前的装备工作
  if (L->status == LUA_OK) {  /* starting a coroutine? */
    // 如果返回0，表示是lua函数中resume的，需要调用luaV_execute，或者就是C函数中resume的
    if (!luaD_precall(L, firstArg - 1, LUA_MULTRET))  /* Lua function? */
      luaV_execute(L);  /* call it */
  }
  // 或者就是从YIELD状态中继续执行，首先将协程的状态置为0，其次判断此时ci的类型
  else {  /* resuming from previous yield */
    lua_assert(L->status == LUA_YIELD);
    L->status = LUA_OK;  /* mark that it is running (again) */
    ci->func = restorestack(L, ci->extra);
    // 判断是否是lua
    if (isLua(ci))  /* yielded inside a hook? */
      // 继续执行lua代码
      luaV_execute(L);  /* just continue running Lua code */
	// 如果不是Lua函数，说明之前是被中断的函数调用，此时调用luaD_poscall 函数继续完成未完的函数操作；
    else {  /* 'common' yield */
      if (ci->u.c.k != NULL) {  /* does it have a continuation function? */
        lua_unlock(L);
        n = (*ci->u.c.k)(L, LUA_YIELD, ci->u.c.ctx); /* call continuation */
        lua_lock(L);
        api_checknelems(L, n);
        firstArg = L->top - n;  /* yield results come from continuation */
      }
      // 完成luaD_precall的调用
      luaD_poscall(L, ci, firstArg, n);  /* finish 'luaD_precall' */
    }
    unroll(L, NULL);  /* run continuation */
  }
}

// 恢复协程执行
LUA_API int lua_resume (lua_State *L, lua_State *from, int nargs) {
  int status;
  unsigned short oldnny = L->nny;  /* save "number of non-yieldable" calls */
  lua_lock(L);
  // status为LUA_OK的话，就是表示开始一个协程
  if (L->status == LUA_OK) {  /* may be starting a coroutine */
    // 协程当前的level必须为base level
    if (L->ci != &L->base_ci)  /* not in base level? */
      return resume_error(L, "cannot resume non-suspended coroutine", nargs);
  }
  else if (L->status != LUA_YIELD)
    return resume_error(L, "cannot resume dead coroutine", nargs);
  // 设置嵌套层数
  L->nCcalls = (from) ? from->nCcalls + 1 : 1;
  if (L->nCcalls >= LUAI_MAXCCALLS)
    return resume_error(L, "C stack overflow", nargs);
  luai_userstateresume(L, nargs);
  L->nny = 0;  /* allow yields */
  api_checknelems(L, (L->status == LUA_OK) ? nargs + 1 : nargs);
  // 调用resume函数
  status = luaD_rawrunprotected(L, resume, &nargs);
  if (status == -1)  /* error calling 'lua_resume'? */
    status = LUA_ERRRUN;
  else {  /* continue running after recoverable errors */
    // 如果是错误状态，就恢复错误
    while (errorstatus(status) && recover(L, status)) {
      /* unroll continuation */
      status = luaD_rawrunprotected(L, unroll, &status);
    }
    // 不可修复的错误
    if (errorstatus(status)) {  /* unrecoverable error? */
      // 将协程设置为死亡状态
      L->status = cast_byte(status);  /* mark thread as 'dead' */
      // 错误信息进栈
      seterrorobj(L, status, L->top);  /* push error message */
      L->ci->top = L->top;
    }
    else lua_assert(status == L->status);  /* normal end or yield */
  }
  // 恢复nny
  L->nny = oldnny;  /* restore 'nny' */
  // 减少调用层级
  L->nCcalls--;
  lua_assert(L->nCcalls == ((from) ? from->nCcalls : 0));
  lua_unlock(L);
  return status;
}

// 是否可以让出，不在主线程中或不在一个无法让出的 C 函数中时，当前协程是可让出的
LUA_API int lua_isyieldable (lua_State *L) {
  return (L->nny == 0);
}


LUA_API int lua_yieldk (lua_State *L, int nresults, lua_KContext ctx,
                        lua_KFunction k) {
  CallInfo *ci = L->ci;
  luai_userstateyield(L, nresults);
  lua_lock(L);
  api_checknelems(L, nresults);
  if (L->nny > 0) {
    if (L != G(L)->mainthread)
      luaG_runerror(L, "attempt to yield across a C-call boundary");
    else
      luaG_runerror(L, "attempt to yield from outside a coroutine");
  }
  // 将执行状态置为LUA_YIELD
  L->status = LUA_YIELD;
  ci->extra = savestack(L, ci->func);  /* save current 'func' */
  // 在lua的钩子里
  if (isLua(ci)) {  /* inside a hook? */
    api_check(L, k == NULL, "hooks cannot continue after yielding");
  }
  else {
      // 是否有一个继续函数
    if ((ci->u.c.k = k) != NULL)  /* is there a continuation? */
      ci->u.c.ctx = ctx;  /* save context */
    // 保护堆栈的结果
    ci->func = L->top - nresults - 1;  /* protect stack below results */
    luaD_throw(L, LUA_YIELD);
  }
  lua_assert(ci->callstatus & CIST_HOOKED);  /* must be inside a hook */
  lua_unlock(L);
  return 0;  /* return to 'luaD_hook' */
}


int luaD_pcall (lua_State *L, Pfunc func, void *u,
                ptrdiff_t old_top, ptrdiff_t ef) {
  int status;
  // 保存原来的调用信息
  CallInfo *old_ci = L->ci;
  // 原来允许的钩子
  lu_byte old_allowhooks = L->allowhook;
  // 原来的yield的计数
  unsigned short old_nny = L->nny;
  // 原来的错误处理函数
  ptrdiff_t old_errfunc = L->errfunc;
  L->errfunc = ef;
  status = luaD_rawrunprotected(L, func, u);
  // 错误发生
  if (status != LUA_OK) {  /* an error occurred? */
	// #define savestack(L,p) ((char *)(p) - (char *)L->stack)
    // #define restorestack(L,n) ((TValue *)((char *)L->stack + (n)))
    // 根据old_top恢复原来的堆栈
    StkId oldtop = restorestack(L, old_top);
    luaF_close(L, oldtop);  /* close possible pending closures */
    // 设置错误obj
    seterrorobj(L, status, oldtop);
    // 回复原来的信息
    L->ci = old_ci;
    L->allowhook = old_allowhooks;
    L->nny = old_nny;
    luaD_shrinkstack(L);
  }
  L->errfunc = old_errfunc;
  return status;
}



/*
** Execute a protected parser.
*/
struct SParser {  /* data to 'f_parser' */
  ZIO *z;
  Mbuffer buff;  /* dynamic structure used by the scanner */
  Dyndata dyd;  /* dynamic structures used by the parser */
  const char *mode;
  const char *name;
};


static void checkmode (lua_State *L, const char *mode, const char *x) {
  if (mode && strchr(mode, x[0]) == NULL) {
    luaO_pushfstring(L,
       "attempt to load a %s chunk (mode is '%s')", x, mode);
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}

// 分析源码
// 完成语法分析之后，产生的字节码相关数据都在LClosure类型的结构体中，然后保存下来，留给后面来执行
static void f_parser (lua_State *L, void *ud) {
  LClosure *cl;
  struct SParser *p = cast(struct SParser *, ud);
  // 得到了第一个字符
  int c = zgetc(p->z);  /* read first character */
  // 二进制
  if (c == LUA_SIGNATURE[0]) {
    checkmode(L, p->mode, "binary");
    cl = luaU_undump(L, p->z, p->name);
  }
  else {
	  // 文本方式
    checkmode(L, p->mode, "text");
    cl = luaY_parser(L, p->z, &p->buff, &p->dyd, p->name, c);
  }
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luaF_initupvals(L, cl);
}

// 被保护的分析函数
int luaD_protectedparser (lua_State *L, ZIO *z, const char *name,
                                        const char *mode) {
  struct SParser p;
  int status;
  // 在分析期间不能yield
  L->nny++;  /* cannot yield during parsing */

  p.z = z; p.name = name; p.mode = mode;
  p.dyd.actvar.arr = NULL; p.dyd.actvar.size = 0;
  p.dyd.gt.arr = NULL; p.dyd.gt.size = 0;
  p.dyd.label.arr = NULL; p.dyd.label.size = 0;
  luaZ_initbuffer(L, &p.buff);
  // 调用f_parser来分析
  status = luaD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  luaM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
  luaM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
  luaM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
  L->nny--;
  return status;
}


