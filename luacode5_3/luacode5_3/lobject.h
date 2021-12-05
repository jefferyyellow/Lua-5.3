/*
** $Id: lobject.h,v 2.117.1.1 2017/04/19 17:39:34 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** Extra tags for non-values
*/
#define LUA_TPROTO	LUA_NUMTAGS		/* function prototypes */
#define LUA_TDEADKEY	(LUA_NUMTAGS+1)		/* removed keys in tables */

/*
** number of all possible tags (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTAGS	(LUA_TPROTO + 2)


/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* value)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/


/*
** LUA_TFUNCTION variants:
** 0 - Lua function
** 1 - light C function
** 2 - regular C function (closure)
*/

/* Variant tags for functions */
// 函数的变量标签
// lua函数(闭包)
#define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))  /* Lua closure */
// 轻量的C函数
#define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))  /* light C function */
// 常规的C函数(C闭包)
#define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))  /* C closure */


/* Variant tags for strings */
// 字符串的变量标签
// 短字符串
#define LUA_TSHRSTR	(LUA_TSTRING | (0 << 4))  /* short strings */
// 长字符串
#define LUA_TLNGSTR	(LUA_TSTRING | (1 << 4))  /* long strings */


/* Variant tags for numbers */
// 数字的变量标签
// 浮点数
#define LUA_TNUMFLT	(LUA_TNUMBER | (0 << 4))  /* float numbers */
// 整数
#define LUA_TNUMINT	(LUA_TNUMBER | (1 << 4))  /* integer numbers */


/* Bit mark for collectable types */
// 需要GC（垃圾回收）的位掩码类型
#define BIT_ISCOLLECTABLE	(1 << 6)

/* mark a tag as collectable */
// 标识一个需要GC(垃圾回收）的标记
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)


/*
** Common type for all collectable objects
*/
typedef struct GCObject GCObject;


/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
// 所有需要GC（垃圾回收）对象的公共头（以宏的形式，需要包含在其他对象中）
// next：   指向下一个GC链表的成员
// tt：     表示数据的类型
// marked:  GC相关的标记字段,用于存储前面提到的几种颜色
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common type has only the common header
*/
// 只包含公共头的公共类型
struct GCObject {
  CommonHeader;
};




/*
** Tagged Values. This is the basic representation of values in Lua,
** an actual value plus a tag with its type.
*/

/*
** Union of all Lua values
*/
// 所有Lua值类型的联合体
typedef union Value {
  GCObject *gc;    // GC（垃圾回收）对象 /* collectable objects */
  void *p;         // 轻量的UserData /* light userdata */
  int b;           // 布尔/* booleans */
  lua_CFunction f; // 轻量的C函数 /* light C functions */
  lua_Integer i;   // 整型 /* integer numbers */
  lua_Number n;    // 浮点型 /* float numbers */
} Value;

// 值类型字段：值和值类型
#define TValuefields	Value value_; int tt_

// 值类型
typedef struct lua_TValue {
  TValuefields;
} TValue;



/* macro defining a nil value */
#define NILCONSTANT	{NULL}, LUA_TNIL

// 得到值的真实保存值信息的部分，不包括值类型
#define val_(o)		((o)->value_)


/* raw type tag of a TValue */
// TValue的元素类型标签
#define rttype(o)	((o)->tt_)

/* tag with no variants (bits 0-3) */
// 标签中不保护变种的部分，也就是取低四位
#define novariant(x)	((x) & 0x0F)

/* type tag of a TValue (bits 0-3 for tags + variant bits 4-5) */
// TValue的类型标签，包括变种，低四位是类型标签，第4-5位表示变种信息
#define ttype(o)	(rttype(o) & 0x3F)

/* type tag of a TValue with no variants (bits 0-3) */
// 不包括变种的类型
#define ttnov(o)	(novariant(rttype(o)))


/* Macros to test type */
// 测试类型的宏
// 大类型比如：LUA_TNUMBER，还包括两个小类型：LUA_TNUMFLT（浮点数），LUA_TNUMINT（整数）
// 小类型是否一致
#define checktag(o,t)		(rttype(o) == (t))
// 大类型是否一致
#define checktype(o,t)		(ttnov(o) == (t))
// 是否数字
#define ttisnumber(o)		checktype((o), LUA_TNUMBER)
#define ttisfloat(o)		checktag((o), LUA_TNUMFLT)
#define ttisinteger(o)		checktag((o), LUA_TNUMINT)

#define ttisnil(o)		checktag((o), LUA_TNIL)
#define ttisboolean(o)		checktag((o), LUA_TBOOLEAN)
#define ttislightuserdata(o)	checktag((o), LUA_TLIGHTUSERDATA)
// 是否是字符串
#define ttisstring(o)		checktype((o), LUA_TSTRING)
// 是否是短字符串
#define ttisshrstring(o)	checktag((o), ctb(LUA_TSHRSTR))
// 是否是长字符串
#define ttislngstring(o)	checktag((o), ctb(LUA_TLNGSTR))
#define ttistable(o)		checktag((o), ctb(LUA_TTABLE))
#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)
#define ttisclosure(o)		((rttype(o) & 0x1F) == LUA_TFUNCTION)
#define ttisCclosure(o)		checktag((o), ctb(LUA_TCCL))
#define ttisLclosure(o)		checktag((o), ctb(LUA_TLCL))
#define ttislcf(o)		checktag((o), LUA_TLCF)
#define ttisfulluserdata(o)	checktag((o), ctb(LUA_TUSERDATA))
#define ttisthread(o)		checktag((o), ctb(LUA_TTHREAD))
#define ttisdeadkey(o)		checktag((o), LUA_TDEADKEY)


/* Macros to access values */
#define ivalue(o)	check_exp(ttisinteger(o), val_(o).i)
#define fltvalue(o)	check_exp(ttisfloat(o), val_(o).n)
#define nvalue(o)	check_exp(ttisnumber(o), \
	(ttisinteger(o) ? cast_num(ivalue(o)) : fltvalue(o)))

// 下面的函数主要用于类型检查和取得Value这一层的信息
// 变量是否是GC（垃圾回收）对象，如果是，得到gc部分信息
#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)

#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
#define tsvalue(o)	check_exp(ttisstring(o), gco2ts(val_(o).gc))
#define uvalue(o)	check_exp(ttisfulluserdata(o), gco2u(val_(o).gc))
#define clvalue(o)	check_exp(ttisclosure(o), gco2cl(val_(o).gc))
#define clLvalue(o)	check_exp(ttisLclosure(o), gco2lcl(val_(o).gc))
#define clCvalue(o)	check_exp(ttisCclosure(o), gco2ccl(val_(o).gc))
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)
#define hvalue(o)	check_exp(ttistable(o), gco2t(val_(o).gc))
#define bvalue(o)	check_exp(ttisboolean(o), val_(o).b)
#define thvalue(o)	check_exp(ttisthread(o), gco2th(val_(o).gc))
/* a dead value may get the 'gc' field, but cannot access its contents */
#define deadvalue(o)	check_exp(ttisdeadkey(o), cast(void *, val_(o).gc))

#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

// 是否需要进行GC(Garbage Collection，垃圾回收）操作：
#define iscollectable(o)	(rttype(o) & BIT_ISCOLLECTABLE)


/* Macros for internal tests */
// 原始的类型（大类）是否一致
#define righttt(obj)		(ttype(obj) == gcvalue(obj)->tt)
// 要是不是GC对象，如果是就不能是死亡对象
#define checkliveness(L,obj) \
	lua_longassert(!iscollectable(obj) || \
		(righttt(obj) && (L == NULL || !isdead(G(L),gcvalue(obj)))))


/* Macros to set values */
// 下面的宏用于设置各种类型的值
// 设置值的类型
#define settt_(o,t)	((o)->tt_=(t))
// 设置浮点数
#define setfltvalue(obj,x) \
  { TValue *io=(obj); val_(io).n=(x); settt_(io, LUA_TNUMFLT); }
// 改变浮点数的值
#define chgfltvalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisfloat(io)); val_(io).n=(x); }

// 设置整数的值
#define setivalue(obj,x) \
  { TValue *io=(obj); val_(io).i=(x); settt_(io, LUA_TNUMINT); }
// 改变整数的值
#define chgivalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisinteger(io)); val_(io).i=(x); }

// 设置nui的值
#define setnilvalue(obj) settt_(obj, LUA_TNIL)
// 设置轻量C函数的值
#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_TLCF); }
// 设置LightUserData的值
#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_TLIGHTUSERDATA); }

// 设置boolean的值
#define setbvalue(obj,x) \
  { TValue *io=(obj); val_(io).b=(x); settt_(io, LUA_TBOOLEAN); }

// 设置一个GC（垃圾回收对象）的值
#define setgcovalue(L,obj,x) \
  { TValue *io = (obj); GCObject *i_g=(x); \
    val_(io).gc = i_g; settt_(io, ctb(i_g->tt)); }

// 设置String
#define setsvalue(L,obj,x) \
  { TValue *io = (obj); TString *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(x_->tt)); \
    checkliveness(L,io); }

// 设置UserData
#define setuvalue(L,obj,x) \
  { TValue *io = (obj); Udata *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TUSERDATA)); \
    checkliveness(L,io); }

// 设置线程数据
#define setthvalue(L,obj,x) \
  { TValue *io = (obj); lua_State *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTHREAD)); \
    checkliveness(L,io); }

// 设置Lua函数（闭包）
#define setclLvalue(L,obj,x) \
  { TValue *io = (obj); LClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TLCL)); \
    checkliveness(L,io); }

// 设置常规的C函数(C闭包)
#define setclCvalue(L,obj,x) \
  { TValue *io = (obj); CClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TCCL)); \
    checkliveness(L,io); }

// 设置Table值
#define sethvalue(L,obj,x) \
  { TValue *io = (obj); Table *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTABLE)); \
    checkliveness(L,io); }

// 设置死亡值
#define setdeadvalue(obj)	settt_(obj, LUA_TDEADKEY)


// 将Obj2指向的值赋值给obj1
#define setobj(L,obj1,obj2) \
	{ TValue *io1=(obj1); *io1 = *(obj2); \
	  (void)L; checkliveness(L,io1); }


/*
** different types of assignments, according to destination
*/

/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj
#define setsvalue2s	setsvalue
#define sethvalue2s	sethvalue
#define setptvalue2s	setptvalue
/* from table to same table */
#define setobjt2t	setobj
/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue

/* to table (define it as an expression to be used in macros) */
#define setobj2t(L,o1,o2)  ((void)L, *(o1)=*(o2), checkliveness(L,(o1)))




/*
** {======================================================
** types and prototypes
** =======================================================
*/

// 堆栈元素的索引
typedef TValue *StkId;  /* index to stack elements */




/*
** Header for string value; string bytes follow the end of this structure
** (aligned according to 'UTString'; see next).
*/
// 字符串值的头； 字符串字节跟在这个结构的末尾（根据“UTString”对齐；见下文）。
typedef struct TString {
  CommonHeader;
  // 短字符串表示“语法保留字”，长字符串表示是否已经计算了hash
  lu_byte extra;  /* reserved words for short strings; "has hash" for longs */
  // 短字符串的长度
  lu_byte shrlen;  /* length for short strings */
  // 字符串的hash
  unsigned int hash;
  // 长字符串不入hash表,之用到长度，短字符串入hash表，用到链接结构
  union {
    // 长字符串的长度
    size_t lnglen;  /* length for long strings */
    // hash表中的链表
    struct TString *hnext;  /* linked list for hash table */
  } u;
} TString;


/*
** Ensures that address after this type is always fully aligned.
*/
// 确保该类型的地址后面都市对齐的
typedef union UTString {
  // 确保字符串最大的对齐
  L_Umaxalign dummy;  /* ensures maximum alignment for strings */
  TString tsv;
} UTString;


/*
** Get the actual string (array of bytes) from a 'TString'.
** (Access to 'extra' ensures that value is really a 'TString'.)
*/
#define getstr(ts)  \
  check_exp(sizeof((ts)->extra), cast(char *, (ts)) + sizeof(UTString))


/* get the actual string (array of bytes) from a Lua value */
// 从Lua 值中获取实际字符串（字节数组）
#define svalue(o)       getstr(tsvalue(o))

/* get string length from 'TString *s' */
#define tsslen(s)	((s)->tt == LUA_TSHRSTR ? (s)->shrlen : (s)->u.lnglen)

/* get string length from 'TValue *o' */
#define vslen(o)	tsslen(tsvalue(o))


/*
** Header for userdata; memory area follows the end of this structure
** (aligned according to 'UUdata'; see next).
*/
typedef struct Udata {
  CommonHeader;
  lu_byte ttuv_;  /* user value's tag */
  struct Table *metatable;
  size_t len;  /* number of bytes */
  union Value user_;  /* user value */
} Udata;


/*
** Ensures that address after this type is always fully aligned.
*/
typedef union UUdata {
  L_Umaxalign dummy;  /* ensures maximum alignment for 'local' udata */
  Udata uv;
} UUdata;


/*
**  Get the address of memory block inside 'Udata'.
** (Access to 'ttuv_' ensures that value is really a 'Udata'.)
*/
#define getudatamem(u)  \
  check_exp(sizeof((u)->ttuv_), (cast(char*, (u)) + sizeof(UUdata)))

#define setuservalue(L,u,o) \
	{ const TValue *io=(o); Udata *iu = (u); \
	  iu->user_ = io->value_; iu->ttuv_ = rttype(io); \
	  checkliveness(L,io); }


#define getuservalue(L,u,o) \
	{ TValue *io=(o); const Udata *iu = (u); \
	  io->value_ = iu->user_; settt_(io, iu->ttuv_); \
	  checkliveness(L,io); }


/*
** Description of an upvalue for function prototypes
*/
typedef struct Upvaldesc {
  TString *name;  /* upvalue name (for debug information) */
  lu_byte instack;  /* whether it is in stack (register) */
  lu_byte idx;  /* index of upvalue (in stack or in outer function's list) */
} Upvaldesc;


/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
// 函数原型的局部变量描述
// 用于调试信息
typedef struct LocVar {
  // 变量名
  TString *varname;
  // 变量激活的第一点
  int startpc;  /* first point where variable is active */
  // 变量死亡的第一点
  int endpc;    /* first point where variable is dead */
} LocVar;


/*
** Function Prototypes
*/
// 函数原型
typedef struct Proto {
  CommonHeader;
  // 固定参数的数量
  lu_byte numparams;  /* number of fixed parameters */
  // 可变参数的函数
  lu_byte is_vararg;
  // 此函数所需的寄存器数量
  lu_byte maxstacksize;  /* number of registers needed by this function */
  // upvalues的数量
  int sizeupvalues;  /* size of 'upvalues' */
  // 常量的数目，存放的是常量数组（也就是k数组）的元素数量，和FuncState中nk的含义一样
  int sizek;  /* size of 'k' */
  int sizecode;
  // lineinfo的大小
  int sizelineinfo;
  // 内嵌函数的数目
  int sizep;  /* size of 'p' */
  int sizelocvars;
  int linedefined;  /* debug information  */
  int lastlinedefined;  /* debug information  */
  // 函数用到的常量
  TValue *k;  /* constants used by the function */
  // 操作码
  Instruction *code;  /* opcodes */
  // 内嵌函数
  struct Proto **p;  /* functions defined inside the function */
  // 从操作码到源代码的映射
  int *lineinfo;  /* map from opcodes to source lines (debug information) */
  // 局部变量信息
  LocVar *locvars;  /* information about local variables (debug information) */
  // upvalue的信息
  Upvaldesc *upvalues;  /* upvalue information */
  // 使用此原型最后创建的闭包
  struct LClosure *cache;  /* last-created closure with this prototype */
  // 源码
  TString  *source;  /* used for debug information */
  GCObject *gclist;
} Proto;



/*
** Lua Upvalues
*/
typedef struct UpVal UpVal;


/*
** Closures
*/

#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist
// C闭包
typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;	  //  C 函数
  TValue upvalue[1];  /* list of upvalues */
} CClosure;


// Lua闭包
typedef struct LClosure {
  ClosureHeader;
  // Lua函数原型,用于存放解析函数体代码之后的指令。
  struct Proto *p;	
  // upvalues列表,用于保存外部引用的局部变量
  UpVal *upvals[1];  /* list of upvalues */
} LClosure;

// 闭包
typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define isLfunction(o)	ttisLclosure(o)

#define getproto(o)	(clLvalue(o)->p)


/*
** Tables
*/
// 表的键
typedef union TKey {
  struct {
    TValuefields;
    // 用于链表，链接下一个node
    int next;  /* for chaining (offset for next node) */
  } nk;
  TValue tvk;
} TKey;


/* copy a value into a key without messing up field 'next' */
// 复制一个值到键里面，注意不处理字段field
#define setnodekey(L,key,obj) \
	{ TKey *k_=(key); const TValue *io_=(obj); \
	  k_->nk.value_ = io_->value_; k_->nk.tt_ = io_->tt_; \
	  (void)L; checkliveness(L,io_); }

// 节点
typedef struct Node {
  // 值
  TValue i_val;
  // 键
  TKey i_key;
} Node;


typedef struct Table {
  CommonHeader;
  // 这是一个byte类型的数据，用于表示这个表中提供了哪些元方法。最开始这个flags是空的，
  // 也就是0 ，当查找一次之后，如果该表中存在某个元方法，那么将该元方法对应的flag bit置为l，
  // 这样下一次查找时只需要比较这个bit就行了。每个元方法对应的bit定义在ltm.h 文件中
  // 缓存元方法是否存在的标记，如果某一位被置1表示该元方法不存在
  lu_byte flags;  /* 1<<p means tagmethod(p) is not present */

  // 该表中以2 为底的散列表大小的对数值。同时由此可知，散列表部分的大小一定是2 的幕，
  // 即如果散列桶数组要扩展的话，也是以每次在原大小基础上乘以2的形式扩展。
  // hash部分的大小的log2
  lu_byte lsizenode;  /* log2 of size of 'node' array */
  // 数组部分的大小
  unsigned int sizearray;  /* size of 'array' array */
  // 数组部分
  TValue *array;  /* array part */
  // 指向该表的散列桶数组起始位置的指针
  // hash部分
  Node *node;
  // 指向该表散列桶数组的最后位置的指针
  Node *lastfree;  /* any free position is before this position */
  // 存放该表的元表
  struct Table *metatable;
  // GC相关的链表
  GCObject *gclist;
} Table;



/*
** 'module' operation for hashing (size is always a power of 2)
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))

// 2的x次方
#define twoto(x)	(1<<(x))
// table中hash桶位的真实长度
#define sizenode(t)	(twoto((t)->lsizenode))


/*
** (address of) a fixed nil value
*/
// 一个固定的nil值的地址
#define luaO_nilobject		(&luaO_nilobject_)


LUAI_DDEC const TValue luaO_nilobject_;

/* size of buffer for 'luaO_utf8esc' function */
#define UTF8BUFFSZ	8

LUAI_FUNC int luaO_int2fb (unsigned int x);
LUAI_FUNC int luaO_fb2int (int x);
LUAI_FUNC int luaO_utf8esc (char *buff, unsigned long x);
LUAI_FUNC int luaO_ceillog2 (unsigned int x);
LUAI_FUNC void luaO_arith (lua_State *L, int op, const TValue *p1,
                           const TValue *p2, TValue *res);
LUAI_FUNC size_t luaO_str2num (const char *s, TValue *o);
LUAI_FUNC int luaO_hexavalue (int c);
LUAI_FUNC void luaO_tostring (lua_State *L, StkId obj);
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t len);


#endif

