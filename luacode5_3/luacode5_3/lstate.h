/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5

// 基本的栈大小
#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */

// 全局的字符串表
typedef struct stringtable {
  TString **hash;
  // 元素的数目
  int nuse;  /* number of elements */
  // 散列桶数目
  int size;
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
typedef struct CallInfo {
  // 栈对应的函数
  StkId func;  /* function index in the stack */
  // 函数的栈顶
  StkId	top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
		// 栈底，就是固定参数开始的地方
      StkId base;  /* base for this function */
	  // 调用函数开始点
      const Instruction *savedpc;
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
	  // 上下文信息
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  ptrdiff_t extra;
  // 函数的期望返回值数目
  short nresults;  /* expected number of results from this function */
  unsigned short callstatus;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
// 调用一个lua函数
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
// 调用调试钩子
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
// 调用正在luaV_execute的新调用上运行
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
// 调用是否是尾调用
#define CIST_TAIL	(1<<5)	/* call was tail called */
// 最后一个钩子叫yielded
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
// 调用一个终结器
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
// 全局状态，所有的线程都共享该状态
typedef struct global_State {
  // Lua的全局内存分配器，用户可以替换成自己的
  lua_Alloc frealloc;  /* function to reallocate memory */
  // 分配器的userdata
  void *ud;         /* auxiliary data to 'frealloc' */
  // 当前使用的内存大小(为实际内存分配器所分配的内存与GCdebt的差值)
  l_mem totalbytes;  /* number of bytes currently allocated - GCdebt */
  // 用于在单次GC之前保存待回收的数据大小。（需要回收的内存数量。）
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
  // GC遍历的内存
  lu_mem GCmemtrav;  /* memory traversed by the GC */
  // 对使用中的非垃圾内存的估计(一个估计值，用于保存实际在用的内存大小。)
  lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */
  // 字符串的hash表, 全局字符串表，几乎每个语言都会对字符串做池化，作成immutable的，Lua的字符串分短字符串和长字符串
  stringtable strt;  /* hash table for strings */
  // 注册表管理全局数据
  TValue l_registry;
  // 哈希算法的随机种子
  unsigned int seed;  /* randomized seed for hashes */
  // 当前的白色见global_State 中的currentwhite，而otherwhite宏用于表示非当前GC将要回收的白色类型。
  lu_byte currentwhite;

  //#define GCSpropagate    0       // 传播阶段：标记对象
  //#define GCSatomic   1           // 原子阶段：一次性标记
  //#define GCSswpallgc 2           // 清扫allgc
  //#define GCSswpfinobj    3       // 清扫finobj
  //#define GCSswptobefnz   4       // 清扫tobefnz
  //#define GCSswpend   5           // 清扫结束
  //#define GCScallfin  6           // 调用终结函数(__gc)
  //#define GCSpause    7           // 停止
  // 当前gc的状态，
  lu_byte gcstate;  /* state of garbage collector */
  //#define KGC_NORMAL	0
  //#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */
  // GC运行的类型
  lu_byte gckind;  /* kind of GC running */
  // GC是否运行
  lu_byte gcrunning;  /* true if GC is running */
  // 存放待GC对象的链表，所有对象创建之后都会放入该链表中。
  // allgc 所有没有被标记为自带终结器的对象链表    
  GCObject *allgc;  /* list of all collectable objects */
  // 待处理的回收数据都存放在rootgc链表中，由于回收阶段不是一次性全部回收这个链表的所有数据，
  // 所以使用这个变量来保存当前回收的位置，下一次从这个位置开始继续回收操作。
  GCObject **sweepgc;  /* current position of sweep in list */
  // 带有终结器的可收集对象列表
  // 新增元素的地方:
  // 1)第一次设置table的元表且元表中含有__gc方法, 此table对象会从allgc链表中移除
  // 2)第一次设置userdata的元表且元表中含有__gc方法时, 此userdata对象会从allgc链表中移除
  GCObject *finobj;  /* list of collectable objects with finalizers */
  // 存放灰色节点的链表。gray 常规灰色等待被访问对象链表
  GCObject *gray;  /* list of gray objects */
  // 存放需要一次性扫描处理的灰色节点链表，也就是说，这个链表上所有数据的处理需要一步到位，不能被打断。
  // grayagain 在原子阶段必须被重新访问的灰色对象链表
  // 包括：在白色barrier中的黑色对象;在繁殖阶段的所有类型弱表;所有线程
  //  新增元素的地方:
  //  1)繁殖阶段遍历弱值表时
  //  2)繁殖阶段遍历弱key表时
  //  3)遍历线程时
  //  4)将黑色结点变灰色向后barrier时
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  // 存放弱表的链表。weak 弱值表对象链表
  //新增元素的地方:
  //  1)非繁殖阶段遍历弱值表含有可能需要清理的值时
  GCObject *weak;  /* list of tables with weak values */
  // ephemeron 蜉蝣对象(弱key表)链表,含有白色->白色结点
  // 新增元素的地方:
  // 1)非繁殖阶段遍历弱key表时有结点的key和value都为白色
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  // allweak 有弱键和/或弱值将被清理的表
  //新增元素的地方:
  //  1)非繁殖阶段遍历弱key表时含有可能需要清理的key且其value标记过
  //  2)遍历表时表是弱key且弱值型
  GCObject *allweak;  /* list of all-weak tables */
  // 要进行GC的用户数据列表
  // tobefnz 将要被释放的对象链表  
  GCObject *tobefnz;  /* list of userdata to be GC */
  // 不能被gc的obj列表，fixedgc 不会被回收的对象链表
  GCObject *fixedgc;  /* list of objects not to be collected */
  // 拥有open upvalues的线程列表
  struct lua_State *twups;  /* list of threads with open upvalues */
  // 每一个GC步骤中，最多多少个finalizers被调用
  unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
  // 用于控制下一轮GC开始的时机。gcpause控制每次完整GC间的间隔(即完整GC频率)
  int gcpause;  /* size of pause between successive GCs */
  // 控制GC 的回收速度。
  int gcstepmul;  /* GC 'granularity' */
  // 全局错误处理响应点(处理不受保护的错误)
  lua_CFunction panic;  /* to be called in unprotected errors */
  // 主线程
  struct lua_State *mainthread;
  // 指向版本号的指针
  const lua_Number *version;  /* pointer to version number */
  // 内存错误信息
  TString *memerrmsg;  /* memory-error message */
  // 元方法的名字数组,metatable的预定义方法名字数组，tm是tag method的缩写
  TString *tmname[TM_N];  /* array with tag-method names */
  // 每个基本类型一个metatable，注意table、userdata等则是每个实例一个metatable。metatable+tag method可以说是整个Lua最重要的Hook机制。
  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types */
  // Lua5.3.4的实现中对于长字符串是单独存储,并使用strcache来缓存,对于短字符串还是像原来一样存储在stringtable中
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
} global_State;


/*
** 'per thread' state
*/
// 每个线程的状态
struct lua_State {
  CommonHeader;
  // 调用信息的数目
  unsigned short nci;  /* number of items in 'ci' list */
  // 状态：运行，挂起
  lu_byte status;
  // 栈顶
  StkId top;  /* first free slot in the stack */
  global_State *l_G;
  // 当前函数的调用信息
  CallInfo *ci;  /* call info for current function */
  // 上一次追踪的pc
  const Instruction *oldpc;  /* last pc traced */
  StkId stack_last;  /* last free slot in the stack */
  // 堆栈起始部分
  StkId stack;  /* stack base */
  // 此堆栈中打开的upvalues列表
  UpVal *openupval;  /* list of open upvalues in this stack */
  GCObject *gclist;
  struct lua_State *twups;  /* list of threads with open upvalues */
  struct lua_longjmp *errorJmp;  /* current error recover point */
  // 第一层的调用信息(C调用Lua)
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
  volatile lua_Hook hook;
  ptrdiff_t errfunc;  /* current error handling function (stack index) */
  int stacksize;
  // 基本的钩子计数，可能会重置到这个计数
  int basehookcount;
  // 实时的钩子计数
  int hookcount;
  // 在堆栈中不能yield的计数
  unsigned short nny;  /* number of non-yieldable calls in stack */
  // 嵌套 C 调用的数量
  unsigned short nCcalls;  /* number of nested C calls */
  l_signalT hookmask;
  lu_byte allowhook;
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
*/
// 所有的垃圾收集器管理的实体的联合体
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
};


#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
// 得到实际分配的总字节数
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)
// 将 GCdebt 设置为保持该值的新值
LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

