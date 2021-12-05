/*
** $Id: lfunc.h,v 2.15.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"

// 得到C闭包的大小
#define sizeCclosure(n)	(cast(int, sizeof(CClosure)) + \
                         cast(int, sizeof(TValue)*((n)-1)))
// 得到Lua闭包的大小
#define sizeLclosure(n)	(cast(int, sizeof(LClosure)) + \
                         cast(int, sizeof(TValue *)*((n)-1)))


/* test whether thread is in 'twups' list */
// 测试线程是否在 'twups' 列表中
#define isintwups(L)	(L->twups != L)


/*
** maximum number of upvalues in a closure (both C and Lua). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL	255


/*
** Upvalues for Lua closures
*/
// Lua闭包的Upvalues
// 我们注意到，这个结构体内部使用了union来存储数据，这说明这个数据结构可能有两种不同的状态： 
// close以及open状态。在close状态下，使用的是TValue类型的数据；而在open状态下，
// 使用的是两个UpVal类型的指针。下面来看看这两种状态分别指代的是什么。
// 所谓的open状态，指的就是被引用到的变量，其所在的函数环境还存在，并没有被销毁，
// 因此这里只需要使用指针引用到相应的变量即可。
// 如果在离开函数外包函数之后再使用内嵌函数，此时对于这个函数而言，引用到的UpValue，变量在离开外包函数时空间被释放了，
// 这个UpValue就是close状态的，此时需要把这个数据的值保存在结构体UpVal的成员TValue value 中。需要注意的是，
// 这个成员的类型已经不是指针了。
struct UpVal {
  // 指向堆栈或者指向它自己的值
  TValue *v;  /* points to stack or to its own value */
  lu_mem refcount;  /* reference counter */
  union {
    struct {  /* (when open) */
      UpVal *next;  /* linked list */
      int touched;  /* mark to avoid cycles with dead threads */
    } open;
    TValue value;  /* the value (when closed) */
  } u;
};

#define upisopen(up)	((up)->v != &(up)->u.value)


LUAI_FUNC Proto *luaF_newproto (lua_State *L);
LUAI_FUNC CClosure *luaF_newCclosure (lua_State *L, int nelems);
LUAI_FUNC LClosure *luaF_newLclosure (lua_State *L, int nelems);
LUAI_FUNC void luaF_initupvals (lua_State *L, LClosure *cl);
LUAI_FUNC UpVal *luaF_findupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_close (lua_State *L, StkId level);
LUAI_FUNC void luaF_freeproto (lua_State *L, Proto *f);
LUAI_FUNC const char *luaF_getlocalname (const Proto *func, int local_number,
                                         int pc);


#endif
