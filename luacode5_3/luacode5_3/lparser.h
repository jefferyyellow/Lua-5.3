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
  // nil类型
  VNIL,  /* constant nil */
  // 表达式是TRUE
  VTRUE,  /* constant true */
  // 表达式是FALSE
  VFALSE,  /* constant false */
  // 表达式是常量类型，expdesc的info字段表示，这个常量是常量表k中的哪个值
  VK,  /* constant in 'k'; info = index of constant in 'k' */
  // 浮点类型
  VKFLT,  /* floating constant; nval = numerical float value */
  // 整数类型
  VKINT,  /* integer constant; nval = numerical integer value */
  // 表达式已经在某个寄存器上了，expdesc的info字段，表示该寄存器的位置
  VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  // 局部变量,expdesc的info字段，表示该local变量，在栈中的位置
  VLOCAL,  /* local variable; info = local register */
  // upvalue变量,expdesc的info字段，表示Upvalue数组的索引
  VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  // 索引变量
  VINDEXED,  /* indexed variable;                               // 索引变量
                ind.vt = whether 't' is register or upvalue;    // 't'是一个寄存器或者一个upvalue
                ind.t = table register or upvalue;              // table寄存器或者upvalue
                ind.idx = key's R/K index */                    // 键的R/K索引
  VJMP,  /* expression is a test/comparison;            // 表达式是测试/比较；
            info = pc of corresponding jump instruction */ // info = 对应跳转指令的pc
  // 表达式可以把结果放到任意的寄存器上，expdesc的info表示的是instruction pc
  VRELOCABLE,  /* expression can put result in any register;
                  info = instruction pc */
  // 达式是函数调用，expdesc中的info字段，表示的是instruction pc
  // 也就是它指向Proto code列表的哪个指令
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
    // 而info根据不同的数据类型各自表示不同的信息,这些信息都可以在expkind enum的注释中看到:
    int info;  /* for generic use */
    struct {  /* for indexed variables (VINDEXED) */
	  // 常量表k或者是寄存器的索引，这个索引指向的值就是被取出值得key
	  // 不论t是Upvalue还是table的索引，它取出的值一般是一个table
      short idx;  /* index (R/K) */
      // 表示table或者是UpVal的索引
      lu_byte t;  /* table (register or upvalue) */
      // 标识上一个字段't'是upvalue(VUPVAL) 还是寄存器(VLOCAL)
      lu_byte vt;  /* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
    } ind;
  } u;
  // 为true退出的补丁列表
  int t;  /* patch list of 'exit when true' */
  // 为false退出的补丁列表
  int f;  /* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
// 局部变量的描述
typedef struct Vardesc {
  // 栈上的变量索引
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
// 分析器使用的动态数据结构
typedef struct Dyndata {
  // 激活的局部变量的列表
  struct {  /* list of active local variables */
    Vardesc *arr;
    // arr中有值的个数
    int n;
    // arr整个的大小
    int size;
  } actvar;
  // 代处理的goto列表
  Labellist gt;  /* list of pending gotos */
  // 激活的标签列表
  Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
// 为给定函数生成代码所需的状态
typedef struct FuncState {
  // 负责保存函数体解析完毕之后生成的指令数据。
  Proto *f;  /* current function header */
  // 包含该函数的函数(外包函数，它指向本函数环境的父函数的FuncState指针。)
  struct FuncState *prev;  /* enclosing function */
  // 词法分析器
  struct LexState *ls;  /* lexical state */
  struct BlockCnt *bl;  /* chain of current blocks */
  // Proto结构的code数组中，下一个可被写入的位置下标
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
  // Proto中的局部变量的数目
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
