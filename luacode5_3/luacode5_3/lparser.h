/*
** $Id: lparser.h,v 1.76.1.1 2017/04/19 17:20:42 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* kinds of variables/expressions */
// 变量/表达式的种类
typedef enum {
  // 当'expdesc'描述一个列表的最后一个表达式时，这种表示一个空列表（所以，没有表达式） 
  VVOID,  /* when 'expdesc' describes the last expression a list,
             this kind means an empty list (so, no expression) */
  VNIL,  /* constant nil */
  VTRUE,  /* constant true */
  VFALSE,  /* constant false */
  VK,  /* constant in 'k'; info = index of constant in 'k' */
  VKFLT,  /* floating constant; nval = numerical float value */
  VKINT,  /* integer constant; nval = numerical integer value */
  // 表达式在固定寄存器中具有其值；info=结果寄存器
  VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  // 局部变量
  VLOCAL,  /* local variable; info = local register */
  // upvalue变量
  VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  // 索引变量
  VINDEXED,  /* indexed variable;                               // 索引变量
                ind.vt = whether 't' is register or upvalue;    // 't'是一个寄存器或者一个upvalue
                ind.t = table register or upvalue;              // table寄存器或者upvalue
                ind.idx = key's R/K index */                    // 键的R/K索引
  VJMP,  /* expression is a test/comparison;
            info = pc of corresponding jump instruction */
  VRELOCABLE,  /* expression can put result in any register;
                  info = instruction pc */
  VCALL,  /* expression is a function call; info = instruction pc */
  VVARARG  /* vararg expression; info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

// 解析表达式的结果会存储在一个临时数据结构expdesc中：
typedef struct expdesc {
  // 变量k表示具体的类型
  expkind k;
  // 后面紧跟的union U根据不同的类型存储的数据有所区分，具体可以看expkind 类型定义后面的注释。
  union {
    lua_Integer ival;    /* for VKINT */
    lua_Number nval;  /* for VKFLT */
    // 一般用途
    int info;  /* for generic use */
    struct {  /* for indexed variables (VINDEXED) */
      short idx;  /* index (R/K) */
      lu_byte t;  /* table (register or upvalue) */
      lu_byte vt;  /* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
    } ind;
  } u;
  int t;  /* patch list of 'exit when true' */
  int f;  /* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
typedef struct Vardesc {
  short idx;  /* variable index in stack */
} Vardesc;


/* description of pending goto statements and label statements */
typedef struct Labeldesc {
  TString *name;  /* label identifier */
  int pc;  /* position in code */
  int line;  /* line where it appeared */
  lu_byte nactvar;  /* local level where it appears in current block */
} Labeldesc;


/* list of labels or gotos */
typedef struct Labellist {
  Labeldesc *arr;  /* array */
  int n;  /* number of entries in use */
  int size;  /* array size */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata {
  struct {  /* list of active local variables */
    Vardesc *arr;
    int n;
    int size;
  } actvar;
  Labellist gt;  /* list of pending gotos */
  Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
// 为给定函数生成代码所需的状态
typedef struct FuncState {
  // 负责保存函数体解析完毕之后生成的指令数据。
  Proto *f;  /* current function header */
  // 包含该函数的函数(它指向本函数环境的父函数的FuncState指针。)
  struct FuncState *prev;  /* enclosing function */
  struct LexState *ls;  /* lexical state */
  struct BlockCnt *bl;  /* chain of current blocks */
  int pc;  /* next position to code (equivalent to 'ncode') */
  // 上一个跳转的标签
  int lasttarget;   /* 'label' of last 'jump label' */
  // 没有确定的跳转到“pc”的列表
  int jpc;  /* list of pending jumps to 'pc' */
  // nk存放的是常量数组（也就是k数组）的元素数量
  int nk;  /* number of elements in 'k' */
  // 函数原型的元素数目
  int np;  /* number of elements in 'p' */
  // 第一个局部变量的索引（在Dyndata数组中）
  int firstlocal;  /* index of first local var (in Dyndata array) */
  // Proto中的locvars的数目
  short nlocvars;  /* number of elements in 'f->locvars' */
  // 激活的局部变量的数目
  lu_byte nactvar;  /* number of active local variables */
  // upvalues的数目
  lu_byte nups;  /* number of upvalues */
  // 第一个空闲的寄存器
  lu_byte freereg;  /* first free register */
} FuncState;


LUAI_FUNC LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                 Dyndata *dyd, const char *name, int firstchar);


#endif
