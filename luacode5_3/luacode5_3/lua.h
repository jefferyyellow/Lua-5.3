/*
** $Id: lua.h,v 1.332.1.2 2018/06/13 16:58:17 roberto Exp $
** Lua - A Scripting Language
** Lua.org, PUC-Rio, Brazil (http://www.lua.org)
** See Copyright Notice at the end of this file
*/


#ifndef lua_h
#define lua_h

#include <stdarg.h>
#include <stddef.h>


#include "luaconf.h"


#define LUA_VERSION_MAJOR	"5"
#define LUA_VERSION_MINOR	"3"
#define LUA_VERSION_NUM		503
#define LUA_VERSION_RELEASE	"5"

#define LUA_VERSION	"Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
#define LUA_RELEASE	LUA_VERSION "." LUA_VERSION_RELEASE
#define LUA_COPYRIGHT	LUA_RELEASE "  Copyright (C) 1994-2018 Lua.org, PUC-Rio"
#define LUA_AUTHORS	"R. Ierusalimschy, L. H. de Figueiredo, W. Celes"


/* mark for precompiled code ('<esc>Lua') */
#define LUA_SIGNATURE	"\x1bLua"

/* option for multiple returns in 'lua_pcall' and 'lua_call' */
#define LUA_MULTRET	(-1)


/*
** Pseudo-indices
** (-LUAI_MAXSTACK is the minimum valid index; we keep some free empty
** space after that to help overflow detection)
*/
// 注册表索引
#define LUA_REGISTRYINDEX	(-LUAI_MAXSTACK - 1000)
#define lua_upvalueindex(i)	(LUA_REGISTRYINDEX - (i))


/* thread status */
#define LUA_OK		0
#define LUA_YIELD	1
#define LUA_ERRRUN	2
#define LUA_ERRSYNTAX	3
#define LUA_ERRMEM	4
#define LUA_ERRGCMM	5
#define LUA_ERRERR	6


typedef struct lua_State lua_State;


/*
** basic types
*/
// 基本类型
// 其中LUA TLIGHTUSERDATA 和LUA TUSERDATA－样，对应的都是void ＊指针，区别在于前者的分
// 配释放由Lua 外部的使用者来完成，而后者则是通过Lua 内部来完成的。换言之，前者不需要Lua
// 去关心它的生存期，由使用者自己去关注，后者则反之。
#define LUA_TNONE		    (-1)            // 无类型      对应数据结构：无

#define LUA_TNIL		    0               // 空类型      对应数据结构：无
#define LUA_TBOOLEAN		1               // 布尔类型    对应数据结构：无
#define LUA_TLIGHTUSERDATA	2               // 指针        对应数据结构：void*
#define LUA_TNUMBER		    3               // 数据        对应数据结构：lua_Number
#define LUA_TSTRING		    4               // 字符串      对应数据结构：TString
#define LUA_TTABLE		    5               // 表          对应数据结构：Table
#define LUA_TFUNCTION		6               // 函数        对应数据结构：CClosure,LClosure
#define LUA_TUSERDATA		7               // 指针        对应数据结构：void*
#define LUA_TTHREAD		    8               // Lua虚拟机、协程 对应数据结构：lua_State

#define LUA_NUMTAGS		    9



/* minimum Lua stack available to a C function */
// C函数可用的最小Lua堆栈
#define LUA_MINSTACK	20


/* predefined values in the registry */
// 注册表里预定义的值
// 主线程的索引
#define LUA_RIDX_MAINTHREAD	1
// 全局表的索引
#define LUA_RIDX_GLOBALS	2
#define LUA_RIDX_LAST		LUA_RIDX_GLOBALS


/* type of numbers in Lua */
typedef LUA_NUMBER lua_Number;


/* type for integer functions */
typedef LUA_INTEGER lua_Integer;

/* unsigned integer type */
typedef LUA_UNSIGNED lua_Unsigned;

/* type for continuation-function contexts */
typedef LUA_KCONTEXT lua_KContext;


/*
** Type for C functions registered with Lua
*/
typedef int (*lua_CFunction) (lua_State *L);

/*
** Type for continuation functions
*/
typedef int (*lua_KFunction) (lua_State *L, int status, lua_KContext ctx);


/*
** Type for functions that read/write blocks when loading/dumping Lua chunks
*/
typedef const char * (*lua_Reader) (lua_State *L, void *ud, size_t *sz);

typedef int (*lua_Writer) (lua_State *L, const void *p, size_t sz, void *ud);


/*
** Type for memory-allocation functions
*/
typedef void * (*lua_Alloc) (void *ud, void *ptr, size_t osize, size_t nsize);



/*
** generic extra include file
*/
#if defined(LUA_USER_H)
#include LUA_USER_H
#endif


/*
** RCS ident string
*/
extern const char lua_ident[];


/*
** state manipulation
*/
LUA_API lua_State *(lua_newstate) (lua_Alloc f, void *ud);
LUA_API void       (lua_close) (lua_State *L);
LUA_API lua_State *(lua_newthread) (lua_State *L);

LUA_API lua_CFunction (lua_atpanic) (lua_State *L, lua_CFunction panicf);


LUA_API const lua_Number *(lua_version) (lua_State *L);


/*
** basic stack manipulation
*/
// 将一个可接受的索引 idx 转换为绝对索引
LUA_API int   (lua_absindex) (lua_State *L, int idx);
// 返回栈顶元素的索引。 因为索引是从 1 开始编号的， 
// 所以这个结果等于栈上的元素个数； 特别指出，0 表示栈为空。
LUA_API int   (lua_gettop) (lua_State *L);
// 参数允许传入任何索引以及 0 。 它将把堆栈的栈顶设为这个索引。 如果新的栈顶比原来的大， 
// 超出部分的新元素将被填为 nil 。 如果 index 为 0 ， 把栈上所有元素移除。
LUA_API void  (lua_settop) (lua_State *L, int idx);
// 将index对应的值拷贝一份到栈顶，然后自增栈顶
LUA_API void  (lua_pushvalue) (lua_State *L, int idx);
// 把从 idx 开始到栈顶的元素轮转 n 个位置。 对于 n 为正数时，轮转方向是向栈顶的； 
// 当 n 为负数时，向栈底方向轮转 -n 个位置。 n 的绝对值不可以比参于轮转的切片长度大。
// 最后索引为n的元素变成了栈顶元素
LUA_API void  (lua_rotate) (lua_State *L, int idx, int n);
// 从索引 fromidx 处复制一个值到一个有效索引 toidx 处，覆盖那里的原有值。 不会影响其它位置的值。
LUA_API void  (lua_copy) (lua_State *L, int fromidx, int toidx);
// 确保堆栈上至少有 n 个额外空位。 如果不能把堆栈扩展到相应的尺寸，函数返回假。 失败的原因包括将把栈扩展到比固定最大尺寸还大 
// （至少是几千个元素）或分配内存失败。 这个函数永远不会缩小堆栈； 如果堆栈已经比需要的大了，那么就保持原样。
LUA_API int   (lua_checkstack) (lua_State *L, int n);
// 交换同一个状态机下不同线程中的值。
// 这个函数会从 from 的栈上弹出 n 个值， 然后把它们压入 to 的栈上。
LUA_API void  (lua_xmove) (lua_State *from, lua_State *to, int n);


/*
** access functions (stack -> C)
*/

LUA_API int             (lua_isnumber) (lua_State *L, int idx);
LUA_API int             (lua_isstring) (lua_State *L, int idx);
LUA_API int             (lua_iscfunction) (lua_State *L, int idx);
LUA_API int             (lua_isinteger) (lua_State *L, int idx);
LUA_API int             (lua_isuserdata) (lua_State *L, int idx);
LUA_API int             (lua_type) (lua_State *L, int idx);
LUA_API const char     *(lua_typename) (lua_State *L, int tp);

LUA_API lua_Number      (lua_tonumberx) (lua_State *L, int idx, int *isnum);
LUA_API lua_Integer     (lua_tointegerx) (lua_State *L, int idx, int *isnum);
LUA_API int             (lua_toboolean) (lua_State *L, int idx);
LUA_API const char     *(lua_tolstring) (lua_State *L, int idx, size_t *len);
LUA_API size_t          (lua_rawlen) (lua_State *L, int idx);
LUA_API lua_CFunction   (lua_tocfunction) (lua_State *L, int idx);
LUA_API void	       *(lua_touserdata) (lua_State *L, int idx);
LUA_API lua_State      *(lua_tothread) (lua_State *L, int idx);
LUA_API const void     *(lua_topointer) (lua_State *L, int idx);


/*
** Comparison and arithmetic functions
*/

#define LUA_OPADD	0	// 加法 (+)      /* ORDER TM, ORDER OP */
#define LUA_OPSUB	1   // 减法 (-)
#define LUA_OPMUL	2   // 乘法 (*)
#define LUA_OPMOD	3   // 取模 (%)
#define LUA_OPPOW	4   // 乘方 (^)
#define LUA_OPDIV	5   // 浮点除法 (/)
#define LUA_OPIDIV	6   // 向下取整的除法 (//)
#define LUA_OPBAND	7   // 按位与 (&)
#define LUA_OPBOR	8   // 按位或 (|)
#define LUA_OPBXOR	9   // 按位异或 (~)
#define LUA_OPSHL	10  // 左移 (<<)
#define LUA_OPSHR	11  // 右移 (>>)
#define LUA_OPUNM	12  // 取负 (一元 -)
#define LUA_OPBNOT	13  // 按位取反 (~)

LUA_API void  (lua_arith) (lua_State *L, int op);

#define LUA_OPEQ	0
#define LUA_OPLT	1
#define LUA_OPLE	2

LUA_API int   (lua_rawequal) (lua_State *L, int idx1, int idx2);
LUA_API int   (lua_compare) (lua_State *L, int idx1, int idx2, int op);


/*
** push functions (C -> stack)
*/
LUA_API void        (lua_pushnil) (lua_State *L);
LUA_API void        (lua_pushnumber) (lua_State *L, lua_Number n);
LUA_API void        (lua_pushinteger) (lua_State *L, lua_Integer n);
LUA_API const char *(lua_pushlstring) (lua_State *L, const char *s, size_t len);
LUA_API const char *(lua_pushstring) (lua_State *L, const char *s);
LUA_API const char *(lua_pushvfstring) (lua_State *L, const char *fmt,
                                                      va_list argp);
LUA_API const char *(lua_pushfstring) (lua_State *L, const char *fmt, ...);
LUA_API void  (lua_pushcclosure) (lua_State *L, lua_CFunction fn, int n);
LUA_API void  (lua_pushboolean) (lua_State *L, int b);
LUA_API void  (lua_pushlightuserdata) (lua_State *L, void *p);
LUA_API int   (lua_pushthread) (lua_State *L);


/*
** get functions (Lua -> stack)
*/
LUA_API int (lua_getglobal) (lua_State *L, const char *name);
LUA_API int (lua_gettable) (lua_State *L, int idx);
LUA_API int (lua_getfield) (lua_State *L, int idx, const char *k);
LUA_API int (lua_geti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawget) (lua_State *L, int idx);
LUA_API int (lua_rawgeti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawgetp) (lua_State *L, int idx, const void *p);

LUA_API void  (lua_createtable) (lua_State *L, int narr, int nrec);
LUA_API void *(lua_newuserdata) (lua_State *L, size_t sz);
LUA_API int   (lua_getmetatable) (lua_State *L, int objindex);
LUA_API int  (lua_getuservalue) (lua_State *L, int idx);


/*
** set functions (stack -> Lua)
*/
LUA_API void  (lua_setglobal) (lua_State *L, const char *name);
LUA_API void  (lua_settable) (lua_State *L, int idx);
LUA_API void  (lua_setfield) (lua_State *L, int idx, const char *k);
LUA_API void  (lua_seti) (lua_State *L, int idx, lua_Integer n);
// 栈索引idx的为表，L->top - 2为键，L->top - 1为值, 设置:表[键]=值
LUA_API void  (lua_rawset) (lua_State *L, int idx);
LUA_API void  (lua_rawseti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawsetp) (lua_State *L, int idx, const void *p);
LUA_API int   (lua_setmetatable) (lua_State *L, int objindex);
LUA_API void  (lua_setuservalue) (lua_State *L, int idx);


/*
** 'load' and 'call' functions (load and run Lua code)
*/
LUA_API void  (lua_callk) (lua_State *L, int nargs, int nresults,
                           lua_KContext ctx, lua_KFunction k);
#define lua_call(L,n,r)		lua_callk(L, (n), (r), 0, NULL)

LUA_API int   (lua_pcallk) (lua_State *L, int nargs, int nresults, int errfunc,
                            lua_KContext ctx, lua_KFunction k);
#define lua_pcall(L,n,r,f)	lua_pcallk(L, (n), (r), (f), 0, NULL)

LUA_API int   (lua_load) (lua_State *L, lua_Reader reader, void *dt,
                          const char *chunkname, const char *mode);

LUA_API int (lua_dump) (lua_State *L, lua_Writer writer, void *data, int strip);


/*
** coroutine functions
*/
LUA_API int  (lua_yieldk)     (lua_State *L, int nresults, lua_KContext ctx,
                               lua_KFunction k);
LUA_API int  (lua_resume)     (lua_State *L, lua_State *from, int narg);
LUA_API int  (lua_status)     (lua_State *L);
// 是否可以让出，不在主线程中或不在一个无法让出的 C 函数中时，当前协程是可让出的
LUA_API int (lua_isyieldable) (lua_State *L);

#define lua_yield(L,n)		lua_yieldk(L, (n), 0, NULL)


/*
** garbage-collection function and options
*/

#define LUA_GCSTOP		0
#define LUA_GCRESTART		1
#define LUA_GCCOLLECT		2
#define LUA_GCCOUNT		3
#define LUA_GCCOUNTB		4
#define LUA_GCSTEP		5
#define LUA_GCSETPAUSE		6
#define LUA_GCSETSTEPMUL	7
#define LUA_GCISRUNNING		9

LUA_API int (lua_gc) (lua_State *L, int what, int data);


/*
** miscellaneous functions
*/

LUA_API int   (lua_error) (lua_State *L);

LUA_API int   (lua_next) (lua_State *L, int idx);

LUA_API void  (lua_concat) (lua_State *L, int n);
LUA_API void  (lua_len)    (lua_State *L, int idx);

LUA_API size_t   (lua_stringtonumber) (lua_State *L, const char *s);

LUA_API lua_Alloc (lua_getallocf) (lua_State *L, void **ud);
LUA_API void      (lua_setallocf) (lua_State *L, lua_Alloc f, void *ud);



/*
** {==============================================================
** some useful macros
** ===============================================================
*/

#define lua_getextraspace(L)	((void *)((char *)(L) - LUA_EXTRASPACE))

#define lua_tonumber(L,i)	lua_tonumberx(L,(i),NULL)
#define lua_tointeger(L,i)	lua_tointegerx(L,(i),NULL)
// 从栈中弹出 n 个元素。
#define lua_pop(L,n)		lua_settop(L, -(n)-1)

#define lua_newtable(L)		lua_createtable(L, 0, 0)

#define lua_register(L,n,f) (lua_pushcfunction(L, (f)), lua_setglobal(L, (n)))

#define lua_pushcfunction(L,f)	lua_pushcclosure(L, (f), 0)

#define lua_isfunction(L,n)	(lua_type(L, (n)) == LUA_TFUNCTION)
#define lua_istable(L,n)	(lua_type(L, (n)) == LUA_TTABLE)
#define lua_islightuserdata(L,n)	(lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
#define lua_isnil(L,n)		(lua_type(L, (n)) == LUA_TNIL)
#define lua_isboolean(L,n)	(lua_type(L, (n)) == LUA_TBOOLEAN)
#define lua_isthread(L,n)	(lua_type(L, (n)) == LUA_TTHREAD)
#define lua_isnone(L,n)		(lua_type(L, (n)) == LUA_TNONE)
// 栈上n的值是否是none或者nil
#define lua_isnoneornil(L, n)	(lua_type(L, (n)) <= 0)
// 这个宏等价于 lua_pushstring， 区别仅在于只能在 s 是一个字面量时才能用它。 它会自动给出字符串的长度。
#define lua_pushliteral(L, s)	lua_pushstring(L, "" s)

#define lua_pushglobaltable(L)  \
	((void)lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS))

#define lua_tostring(L,i)	lua_tolstring(L, (i), NULL)

// 把栈顶元素移动到指定的有效索引处， 依次移动这个索引之上的元素。 
// 不要用伪索引来调用这个函数， 因为伪索引没有真正指向栈上的位置。
#define lua_insert(L,idx)	lua_rotate(L, (idx), 1)
// 从堆栈中删除指定索引的值（将idx的值旋转到栈顶，然后出栈）
#define lua_remove(L,idx)	(lua_rotate(L, (idx), -1), lua_pop(L, 1))
// 把栈顶元素放置到给定位置而不移动其它元素 （因此覆盖了那个位置处的值），然后将栈顶元素弹出。
#define lua_replace(L,idx)	(lua_copy(L, -1, (idx)), lua_pop(L, 1))

/* }============================================================== */


/*
** {==============================================================
** compatibility macros for unsigned conversions
** ===============================================================
*/
#if defined(LUA_COMPAT_APIINTCASTS)

#define lua_pushunsigned(L,n)	lua_pushinteger(L, (lua_Integer)(n))
#define lua_tounsignedx(L,i,is)	((lua_Unsigned)lua_tointegerx(L,i,is))
#define lua_tounsigned(L,i)	lua_tounsignedx(L,(i),NULL)

#endif
/* }============================================================== */

/*
** {======================================================================
** Debug API
** =======================================================================
*/


/*
** Event codes
*/
#define LUA_HOOKCALL	0
#define LUA_HOOKRET	1
#define LUA_HOOKLINE	2
#define LUA_HOOKCOUNT	3
#define LUA_HOOKTAILCALL 4


/*
** Event masks
*/
// 事件掩码
// 在函数被调用时触发
#define LUA_MASKCALL	(1 << LUA_HOOKCALL)
// 在函数返回时触发
#define LUA_MASKRET	(1 << LUA_HOOKRET)
// 在每执行一行时触发
#define LUA_MASKLINE	(1 << LUA_HOOKLINE)
// 每执行count条Lua指令触发一次，这里的count在lua_sethook函数的第三个参数中传人。使用其他hook类型时，该参数无效。
#define LUA_MASKCOUNT	(1 << LUA_HOOKCOUNT)

typedef struct lua_Debug lua_Debug;  /* activation record */


/* Functions to be called by the debugger in specific events */
typedef void (*lua_Hook) (lua_State *L, lua_Debug *ar);

// 获取解释器的运行时栈的信息。
// 这个函数用正在运行中的指定层次处函数的 活动记录 来填写 lua_Debug 结构的一部分。 
// 0 层表示当前运行的函数， n + 1 层的函数就是调用第 n 层 （尾调用例外，它不算在栈层次中）
// 函数的那一个。 如果没有错误， lua_getstack 返回 1 ； 当调用传入的层次大于堆栈深度的时候，返回 0 。
LUA_API int (lua_getstack) (lua_State *L, int level, lua_Debug *ar);
LUA_API int (lua_getinfo) (lua_State *L, const char *what, lua_Debug *ar);
LUA_API const char *(lua_getlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_setlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_getupvalue) (lua_State *L, int funcindex, int n);
LUA_API const char *(lua_setupvalue) (lua_State *L, int funcindex, int n);

LUA_API void *(lua_upvalueid) (lua_State *L, int fidx, int n);
LUA_API void  (lua_upvaluejoin) (lua_State *L, int fidx1, int n1,
                                               int fidx2, int n2);

LUA_API void (lua_sethook) (lua_State *L, lua_Hook func, int mask, int count);
LUA_API lua_Hook (lua_gethook) (lua_State *L);
LUA_API int (lua_gethookmask) (lua_State *L);
LUA_API int (lua_gethookcount) (lua_State *L);


struct lua_Debug {
  // 用于表示触发hook的事件，事件类型就是前面提到的几个宏。
  int event;
  // 当前所在函数的名
  const char *name;	/* (n) */
  // name域的含义。可能的取值为：global、local、method、field或者空字符串。空字符串意味着Lua无法找到这个函数名。
  const char *namewhat;	/* (n) 'global', 'local', 'field', 'method' */
  // 函数类型。如果foo是普通的Lua函数，结果为Lua；如果是C函数，结果为C；如果是Lua的主代码段，结果为main。
  const char *what;	/* (S) 'Lua', 'C', 'main', 'tail' */
  // 函数的定义位置。如果函数在字符串内被定义（通过loadstring函数）,source就是该字符串，如果函数在文件中被定义，source就是带＠前缀的文件名。
  const char *source;	/* (S) */
  // 当前所在行号。
  int currentline;	/* (l) */
  // source中函数被定义处的行号
  int linedefined;	/* (S) */
  // 该函数最后一行代码在源代码中的行号。
  int lastlinedefined;	/* (S) */
  // 该函数的UpValue的数量。
  unsigned char nups;	/* (u) number of upvalues */
  // 该函数参数的数量
  unsigned char nparams;/* (u) number of parameters */
  // 是否是可变参数
  char isvararg;        /* (u) */
  // 是否是尾调用
  char istailcall;	/* (t) */
  // source 的简短版本（ 60个字符以内），对错误信息很有用
  char short_src[LUA_IDSIZE]; /* (S) */
  /* private part */
  // 激活的函数
  struct CallInfo *i_ci;  /* active function */
};

/* }====================================================================== */


/******************************************************************************
* Copyright (C) 1994-2018 Lua.org, PUC-Rio.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/


#endif
