/*
** $Id: lparser.c,v 2.155.1.2 2017/04/29 18:11:40 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



/* maximum number of local variables per function (must be smaller
   than 250, due to the bytecode format) */
// 每个函数最大的局部变量数目（由于字节码格式的原因，必须小于250）
#define MAXVARS		200

// 多个返回值
#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
// 因为所有的字符串都被扫描器统一了，解析器可以使用指针相等来实现字符串相等的比较
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
*/
// 块列表的节点（活动块列表）
typedef struct BlockCnt {
  // 前一个列表
  struct BlockCnt *previous;  /* chain */
  // 代码块中的第一个标签的索引
  int firstlabel;  /* index of first label in this block */
  // 代码块中第一个待处理的go to语句的标签
  int firstgoto;  /* index of first pending goto in this block */
  // 程序块外面活跃的局部变量
  lu_byte nactvar;  /* # active locals outside the block */
  // 程序块中的某个变量是upvalue的话为true
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  // 程序块是一个循环的话为true
  lu_byte isloop;  /* true if 'block' is a loop */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement (LexState *ls);
static void expr (LexState *ls, expdesc *v);


/* semantic error */
// 报语义错误
static l_noret semerror (LexState *ls, const char *msg) {
  ls->t.token = 0;  /* remove "near <token>" from final message */
  luaX_syntaxerror(ls, msg);
}

// 报错token的类型应该为指定类型
static l_noret error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}

// 超限错误
static l_noret errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  // 格式化错误信息
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  // 报语法错误
  luaX_syntaxerror(fs->ls, msg);
}

// 超出限制检查，如果超限就报错
static void checklimit (FuncState *fs, int v, int l, const char *what) {
  if (v > l) errorlimit(fs, l, what);
}

// 测试当前token是否为c，如果为c,取下一个token
static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}

// ls token对应的类型是否为c,如果不为c，报错token的类型应该为指定类型
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}

// 检查当前的token是c类型，然后取下一个token
static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


// 检查当前是否为what，如果不是就根据情况报错
static void check_match (LexState *ls, int what, int who, int where) {
  if (!testnext(ls, what)) {
    // 如果是同一行
    if (where == ls->linenumber)
      error_expected(ls, what);
    // 不同行的语法错误提示
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             "%s expected (to close %s at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}

// 检查变量名，返回变量名字符串，然后读取下一个token
static TString *str_checkname (LexState *ls) {
  TString *ts;
  // 校验变量名类型
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  // 读取下一个token
  luaX_next(ls);
  return ts;
}

// 初始化表达式结构
static void init_exp (expdesc *e, expkind k, int i) {
  // 设置f和t都是不跳转
  e->f = e->t = NO_JUMP;
  // 设置表达式类型
  e->k = k;
  // 参数
  e->u.info = i;
}

// 字符串的字节码 
static void codestring (LexState *ls, expdesc *e, TString *s) {
  // 将字符串加入常量表，返回常量表的索引，然后初始化表达式结构
  init_exp(e, VK, luaK_stringK(ls->fs, s));
}

// 检查变量名，然后编码变量名
static void checkname (LexState *ls, expdesc *e) {
  codestring(ls, e, str_checkname(ls));
}

// 注册新的局部变量
static int registerlocalvar (LexState *ls, TString *varname) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  // 检查是否需要扩展局部变量数组
  luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  // 如果扩展了局部变量数组，将新扩展的部分的变量名都设置为NULL
  while (oldsize < f->sizelocvars)
    f->locvars[oldsize++].varname = NULL;
  // 设置新的局部变量
  f->locvars[fs->nlocvars].varname = varname;
  luaC_objbarrier(ls->L, f, varname);
  return fs->nlocvars++;
}

// 新建一个局部变量
static void new_localvar (LexState *ls, TString *name) {
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  // 注册新的局部变量，reg是栈上的索引
  int reg = registerlocalvar(ls, name);
  // 检查当前函数的局部变量是否超过限制
  checklimit(fs, dyd->actvar.n + 1 - fs->firstlocal,
                  MAXVARS, "local variables");
  // 检查是否需要扩张局部变量的列表
  luaM_growvector(ls->L, dyd->actvar.arr, dyd->actvar.n + 1,
                  dyd->actvar.size, Vardesc, MAX_INT, "local variables");
  // 放入局部变量列表中
  dyd->actvar.arr[dyd->actvar.n++].idx = cast(short, reg);
}

// 创建指定名字的新的局部变量
static void new_localvarliteral_ (LexState *ls, const char *name, size_t sz) {
  new_localvar(ls, luaX_newstring(ls, name, sz));
}
// 创建指定名字的新的局部变量
#define new_localvarliteral(ls,v) \
	new_localvarliteral_(ls, "" v, (sizeof(v)/sizeof(char))-1)

// 通过索引，得到局部变量
static LocVar *getlocvar (FuncState *fs, int i) {
  // 局部变量的索引
  int idx = fs->ls->dyd->actvar.arr[fs->firstlocal + i].idx;
  lua_assert(idx < fs->nlocvars);
  return &fs->f->locvars[idx];
}

// 调整局部变量
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  // 调整局部变量的数目
  fs->nactvar = cast_byte(fs->nactvar + nvars);
  // 调整局部变量开始的字节码记录
  for (; nvars; nvars--) {
    getlocvar(fs, fs->nactvar - nvars)->startpc = fs->pc;
  }
}

// 删除局部变量
static void removevars (FuncState *fs, int tolevel) {
  // 调整激活的数目,(fs->nactvar - tolevel)为减少的数目
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  // 设置删除部分的变量作用域结束的指令处
  while (fs->nactvar > tolevel)
    getlocvar(fs, --fs->nactvar)->endpc = fs->pc;
}

// 根据名字搜索upvalue，找到就返回upvalues中的索引，没找到就返回-1，
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  // 遍历所有的upvalue
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}

// 分配一个新的upvalue
static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  // 检查是否超出限制
  checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  // 赋值
  f->upvalues[fs->nups].instack = (v->k == VLOCAL);
  f->upvalues[fs->nups].idx = cast_byte(v->u.info);
  f->upvalues[fs->nups].name = name;
  luaC_objbarrier(fs->ls->L, f, name);
  return fs->nups++;
}

// 搜索fs环境中的局部变量，找到就返回局部变量的索引，没找到就返回-1
static int searchvar (FuncState *fs, TString *n) {
  int i;
  // 遍历所有的局部变量，通过名字比较
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    if (eqstr(n, getlocvar(fs, i)->varname))
      return i;
  }
  // 没找到返回-1
  return -1;  /* not found */
}


/*
  Mark block where variable at given level was defined
  (to emit close instructions later).
*/
// 标记在给定级别定义变量的块（稍后发出关闭指令）。
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
}


/*
  Find variable with given name 'n'. If it is an upvalue, add this
  upvalue into all intermediate functions.
*/
// 查找具有给定名称“n”的变量。 如果它是一个upvalue，添加这个upvalue到所有中间函数。
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  // 没有更多层了
  if (fs == NULL)  /* no more levels? */
    init_exp(var, VVOID, 0);  /* default is global */
  else {
    // 搜索变量，返回值是局部变量的索引，如果不在fs环境中就返回-1
    int v = searchvar(fs, n);  /* look up locals at current level */
    // 是局部变量
    if (v >= 0) {  /* found? */
      // 初始化为局部变量
      init_exp(var, VLOCAL, v);  /* variable is local */
      // 局部变量作为一个upval使用
      if (!base)
        markupval(fs, v);  /* local will be used as an upval */
    }
    // 这一层局部变量没有找到，那就尝试upvalues
    else {  /* not found as local at current level; try upvalues */
        // 当前层里面搜索upvalue
      int idx = searchupvalue(fs, n);  /* try existing upvalues */
      // 没有找到
      if (idx < 0) {  /* not found? */
        // 递归上一层去找
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
        // 如果是全局变量就直接返回
        if (var->k == VVOID)  /* not found? */
          return;  /* it is a global */
        /* else was LOCAL or UPVAL */
        // 创建一个新的
        idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
      }
      // 赋值到对应索引的upvalue
      init_exp(var, VUPVAL, idx);  /* new or old upvalue */
    }
  }
}


static void singlevar (LexState *ls, expdesc *var) {
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  // 查找具有给定名称“varname”的变量，初始化
  singlevaraux(fs, varname, var, 1);
  // 全局名字
  if (var->k == VVOID) {  /* global name? */
    expdesc key;
    // 环境变量名
    singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
    lua_assert(var->k != VVOID);  /* this one must exist */
    codestring(ls, &key, varname);  /* key is variable name */
    luaK_indexed(fs, var, &key);  /* env[varname] */
  }
}

// 用于根据等号两边变量和表达式的数量来调整赋值。具体来说，
// 在上面这个例子中，当变量数量多于等号右边的表达式数量时，会将多余的变量置为NIL
// nvars：变量的数目
// nexps：表达式返回的值的数目
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;

  int extra = nvars - nexps;
  // 多个返回值
  if (hasmultret(e->k)) {
    extra++;  /* includes call itself */
    if (extra < 0) extra = 0;
    // 设置返回值
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
    if (extra > 1) luaK_reserveregs(fs, extra-1);
  }
  else {
    if (e->k != VVOID) luaK_exp2nextreg(fs, e);  /* close last expression */
    if (extra > 0) {
      int reg = fs->freereg;
      // 预留extra个寄存器
      luaK_reserveregs(fs, extra);
      // 将不足的设置为nil
      luaK_nil(fs, reg, extra);
    }
  }
  // 将多余的返回值从寄存器中移除
  if (nexps > nvars)
    ls->fs->freereg -= nexps - nvars;  /* remove extra values */
}

// 进入C函数调用，增加调用栈中C函数的调用深度（层数）
static void enterlevel (LexState *ls) {
  lua_State *L = ls->L;
  ++L->nCcalls;
  // 校验不能超过最大的调用深度
  checklimit(ls->fs, L->nCcalls, LUAI_MAXCCALLS, "C levels");
}

// 离开C函数调用，减少调用堆栈中C函数的调用深度（层数）
#define leavelevel(ls)	((ls)->L->nCcalls--)

// 如果一个goto标签已经存在的，就应用到以前的语句中，然后把刚才加入的删除
static void closegoto (LexState *ls, int g, Labeldesc *label) {
  int i;
  FuncState *fs = ls->fs;
  Labellist *gl = &ls->dyd->gt;
  Labeldesc *gt = &gl->arr[g];
  lua_assert(eqstr(gt->name, label->name));
  // 如果是前面跳到后面的，并且还来close，报语义错误
  if (gt->nactvar < label->nactvar) {
    TString *vname = getlocvar(fs, gt->nactvar)->varname;
    const char *msg = luaO_pushfstring(ls->L,
      "<goto %s> at line %d jumps into the scope of local '%s'",
      getstr(gt->name), gt->line, getstr(vname));
    semerror(ls, msg);
  }
  // 回填跳转到‘target'的所有跳转列表的项
  luaK_patchlist(fs, gt->pc, label->pc);
  /* remove goto from pending list */
  // 将未处理的最后一个删除掉
  for (i = g; i < gl->n - 1; i++)
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** try to close a goto with existing labels; this solves backward jumps
*/
// 尝试使用现有标签关闭goto； 这解决了向后跳跃
static int findlabel (LexState *ls, int g) {
  int i;
  BlockCnt *bl = ls->fs->bl;
  Dyndata *dyd = ls->dyd;
  Labeldesc *gt = &dyd->gt.arr[g];
  /* check labels in current block for a match */
  // 检查当前块中的标签是否匹配
  for (i = bl->firstlabel; i < dyd->label.n; i++) {
    Labeldesc *lb = &dyd->label.arr[i];
    // 名字相等
    if (eqstr(lb->name, gt->name)) {  /* correct label? */
      // 如果gt再后面，需要
      if (gt->nactvar > lb->nactvar &&
          (bl->upval || dyd->label.n > bl->firstlabel))
        // 将“列表”中的所有跳转路径的close upvalues限制最大为指定的level
        luaK_patchclose(ls->fs, gt->pc, lb->nactvar);
      // 如果一个goto标签已经存在的，就应用到以前的语句中，然后把刚才加入的删除
      closegoto(ls, g, lb);  /* close it */
      return 1;
    }
  }
  return 0;  /* label not found; cannot close goto */
}

// 创建一个新的标签入口
static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  // 是否需要扩展数组
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  // 当前激活的局部变量
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


/*
** check whether new label 'lb' matches any pending gotos in current
** block; solves forward jumps
*/
// 检查新标签 'lb' 是否与当前块中的任何未决 gotos 匹配； 解决向前跳跃
static void findgotos (LexState *ls, Labeldesc *lb) {
  Labellist *gl = &ls->dyd->gt;
  int i = ls->fs->bl->firstgoto;
  while (i < gl->n) {
    // 是否与当前的标签匹配,如果匹配就关闭
    if (eqstr(gl->arr[i].name, lb->name))
      closegoto(ls, i, lb);
    else
      i++;
  }
}


/*
** export pending gotos to outer level, to check them against
** outer labels; if the block being exited has upvalues, and
** the goto exits the scope of any variable (which can be the
** upvalue), close those variables being exited.
*/
// 将挂起的 goto 导出到外部级别，以根据外部标签检查它们； 如果正在退出的块有upvalues，
// 并且goto退出任何变量（可以是upvalues）的范围，则关闭正在退出的那些变量。
static void movegotosout (FuncState *fs, BlockCnt *bl) {
  int i = bl->firstgoto;
  Labellist *gl = &fs->ls->dyd->gt;
  /* correct pending gotos to current block and try to close it
     with visible labels */
  // 遍历退出块未处理的goto标签
  while (i < gl->n) {
    Labeldesc *gt = &gl->arr[i];
    // gt所在的块外面的激活的局部变量还多
    if (gt->nactvar > bl->nactvar) {
      // 退出的块中有upvalue,限制close upvalue的值为bl->nactvar
      if (bl->upval)
        luaK_patchclose(fs, gt->pc, bl->nactvar);
      // 限制其到当前退出块外面激活的局部变量
      gt->nactvar = bl->nactvar;
    }
    // 尝试使用现有标签关闭；
    if (!findlabel(fs->ls, i))
      i++;  /* move to next one */
  }
}

// 进入代码块，初始化代码块数据结构
static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  bl->previous = fs->bl;
  fs->bl = bl;
  lua_assert(fs->freereg == fs->nactvar);
}


/*
** create a label named 'break' to resolve break statements
*/
// 创建一个名为“break”的标签来解析 break 语句 
static void breaklabel (LexState *ls) {
  // 创建break标签
  TString *n = luaS_new(ls->L, "break");
  int l = newlabelentry(ls, &ls->dyd->label, n, 0, ls->fs->pc);
  // 处理当前块中未决的标签
  findgotos(ls, &ls->dyd->label.arr[l]);
}

/*
** generates an error for an undefined 'goto'; choose appropriate
** message when label name is a reserved word (which can only be 'break')
*/
// 为未定义的“goto”生成错误； 当标签名称为保留字（只能是'break'）时选择合适的信息
static l_noret undefgoto (LexState *ls, Labeldesc *gt) {
  const char *msg = isreserved(gt->name)
                    ? "<%s> at line %d not inside a loop"
                    : "no visible label '%s' for <goto> at line %d";
  msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line);
  semerror(ls, msg);
}

// 离开当前的代码块
static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  LexState *ls = fs->ls;
  // 前一个代码块存在，并且当前的代码块中有upvalue
  if (bl->previous && bl->upval) {
    /* create a 'jump to here' to close upvalues */
    // 创建一个原地跳转指令
    int j = luaK_jump(fs);
    // 将“列表”中的所有跳转路径的close upvalues限制最大为指定的bl->nactvar
    luaK_patchclose(fs, j, bl->nactvar);
    // 将j加入跳转到当前的列表中
    luaK_patchtohere(fs, j);
  }
  // 如果是一个循环代码块
  if (bl->isloop)
    // 关闭未处理的break标签
    breaklabel(ls);  /* close pending breaks */
  // 返回到前一个代码块
  fs->bl = bl->previous;
  // 删除代码块中的局部变量
  removevars(fs, bl->nactvar);
  lua_assert(bl->nactvar == fs->nactvar);
  // 设置空闲寄存器的索引
  fs->freereg = fs->nactvar;  /* free registers */
  // 删除局部的标签
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
  // 如果当前的块还是一个内部块,将未处理的goto标签的外层
  if (bl->previous)  /* inner block? */
    movegotosout(fs, bl);  /* update pending gotos to outer block */
  // 如果已经是最外层的代码块了，还有未决的，报错
  else if (bl->firstgoto < ls->dyd->gt.n)  /* pending gotos in outer block? */
    undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
}


/*
** adds a new prototype into list of prototypes
*/
// 创建一个新的函数原型（将新原型添加到原型列表中）
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  // 需要增长
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  // 创建一个新的函数原型,并加入原型列表
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction must use the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.
*/
// 在父函数中创建新闭包的代码指令。OP_CLOSURE指令必须使用最后一个可用的寄存器，
// 这样，如果它调用GC，GC就会知道当时正在使用哪些寄存器。
static void codeclosure (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs->prev;
  init_exp(v, VRELOCABLE, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  // 确保表达式的结果（包括从它的跳转表的结果）保存在下一个可用的寄存器中
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}

// 处理函数FuncState的信息
static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  Proto *f;
  // FuncState的成员prev指针指向其父函数的FuncState指针,
  // 链接链表中的funcstates
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;

  fs->pc = 0;
  fs->lasttarget = 0;
  fs->jpc = NO_JUMP;
  fs->freereg = 0;
  fs->nk = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->nlocvars = 0;
  fs->nactvar = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->bl = NULL;
  f = fs->f;
  f->source = ls->source;
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  // 进入代码块，初始化代码块数据结构
  enterblock(fs, bl, 0);
}

// 函数结束处理，用于将最后分析的结果保存到Proto结构体中
static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  // 编码一个返回指令，处理函数返回结果
  luaK_ret(fs, 0, 0);  /* final return */
  // 离开代码块
  leaveblock(fs);
  luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
  f->sizecode = fs->pc;
  luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->pc, int);
  f->sizelineinfo = fs->pc;
  luaM_reallocvector(L, f->k, f->sizek, fs->nk, TValue);
  f->sizek = fs->nk;
  luaM_reallocvector(L, f->p, f->sizep, fs->np, Proto *);
  f->sizep = fs->np;
  luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
  f->sizelocvars = fs->nlocvars;
  luaM_reallocvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  f->sizeupvalues = fs->nups;
  lua_assert(fs->bl == NULL);
  // 切回到上一个
  ls->fs = fs->prev;
  luaC_checkGC(L);
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
// 检查当前令牌是否在块的后续集中。 'until'关闭句法块，但不关闭作用域，因此单独处理。
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
      return 1;
    case TK_UNTIL: return withuntil;
    default: return 0;
  }
}

// 分析语句列表
static void statlist (LexState *ls) {
  /* statlist -> { stat [';'] } */
  while (!block_follow(ls, 1)) {
    // 返回语句
    if (ls->t.token == TK_RETURN) {
      // 分析语句
      statement(ls);
      return;  /* 'return' must be last statement */
    }
    // 分析语句
    statement(ls);
  }
}

// 表字段解析
static void fieldsel (LexState *ls, expdesc *v) {
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  // 确保表达式的结果要么在寄存器上，要么在一个upvalue上 
  luaK_exp2anyregup(fs, v);
  // 跳过点号（.）或者冒号（：）
  luaX_next(ls);  /* skip the dot or colon */
  // 检查字段名
  checkname(ls, &key);
  // 取字段t[name]
  luaK_indexed(fs, v, &key);
}

// 解析一个以变量为键的工作在yindex函数中进行，
// 解析变量形成表达式相关的expdesc 结构体；
// 根据不同的表达式类型将表达式的值存入寄存器。
//
// 解析table中的索引方式（[]方式）
static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  // 跳过token [
  luaX_next(ls);  /* skip the '[' */
  // 解析中括号中的表达式的值
  expr(ls, v);
  // 确保最终表达式结果在寄存器中或者是常量 
  luaK_exp2val(ls->fs, v);
  // 校验后面的]
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

// 用于表的构造控制
struct ConsControl {
  // 最后读取的表达式
  expdesc v;  /* last list item read */
  // 表表达式
  expdesc *t;  /* table descriptor */
  // hash部分的元素数目
  int nh;  /* total number of 'record' elements */
  // 数组部分的元素数目
  int na;  /* total number of array elements */
  // 有待存储的数组元素
  int tostore;  /* number of array elements pending to be stored */
};

// 初始化表Hash部分的代码
static void recfield (LexState *ls, struct ConsControl *cc) {
  /* recfield -> (NAME | '['exp1']') = exp1 */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc key, val;
  int rkkey;
  // 常量为键值
  if (ls->t.token == TK_NAME) {
    // hash部分数目是否超限
    checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    // 检查键名，然后编码变量名
    checkname(ls, &key);
  }
  else  /* ls->t.token == '[' */
    // 得到中括号的键值
    yindex(ls, &key);
  // 增加在hash中的数量
  cc->nh++;
  checknext(ls, '=');
  // 得到key常量在常量数组中的索引，根据这个值调用luaK_exp2RK函数生成RK值。
  rkkey = luaK_exp2RK(fs, &key);
  // 得到value表达式的索引
  expr(ls, &val);
  // 将前两步的值以及表在寄存器中的索引，写入OP_SETTABLE的参数中。
  luaK_codeABC(fs, OP_SETTABLE, cc->t->u.info, rkkey, luaK_exp2RK(fs, &val));
  // 是否寄存器
  fs->freereg = reg;  /* free registers */
}

// 调用closelistfield 。从这个函数的命名可以看出，它做的工作是针对数组部分的
// 当数组部分积攒到一定程度时，使用OP_SETLIST（以一个基地址和数量来将数据写入表的数组部分）写入表的数组部分
static void closelistfield (FuncState *fs, struct ConsControl *cc) {
  // 没有数组元素，直接返回
  if (cc->v.k == VVOID) return;  /* there is no list item */
  // 调用luaK_exp2nextreg将前面得到的ConsControl结构体中成员v的信息存入寄存器中。
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  // 如果此时tostore成员的值等于LFIELDS_PER_FLUSH，那么生成一个OP_SETLIST指令，用于
  // 将当前寄存器上的数据写入表的数组部分。需要注意的是，这个地方存取的数据在栈上
  // 的位置是紧跟着OP_NEWTABLE指令中的参数A在栈上的位置，而从前面对OP_NEWTABLE 指令
  // 格式的解释可以知道， OP_NEWTABLE指令的参数A存放的是新创建的表在栈上的位置，这
  // 样的话使用一个参数既可以得到表的地址，又可以知道待存入的数据是哪些。之所以需
  // 要限制每次调用OP_SETLIST指令中的数据量不超过LFIELDS_PER_FLUSH，是因为如果不做
  // 这个限制，会导致数组部分数据过多时，占用过多的寄存器，而Lua栈对寄存器数量是有限制的。
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    // 积累了一定量(tostore)的数据，一次性放入nar中
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->tostore = 0;  /* no more items pending */
  }
}

// 将最后没有满LFIELDS_PER_FLUSH个的数组部分写入
static void lastlistfield (FuncState *fs, struct ConsControl *cc) {
  // 如果没有待写入的部分，直接返回
  if (cc->tostore == 0) return;
  // 多返回值
  if (hasmultret(cc->v.k)) {
    // 将表达式的值设置为多返回值
    luaK_setmultret(fs, &cc->v);
    // 写入na起的多个
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    // 如果最后一个不是空
    if (cc->v.k != VVOID)
      // 确保表达式的结果（包括从它的跳转表的结果）保存在下一个可用的寄存器中
      luaK_exp2nextreg(fs, &cc->v);
    // 将剩下的部分写入
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
}

// 数组部分的构造
// 调用expr 函数解析这个表达式，得到对应的ConsControl 结构体中成员V 的数据。前面提
// 过，这个对象会暂存表构造过程中当前表达式的结果。
// 检查当前表中数组部分的数据梳理是否超过限制了。
// 依次将ConsControl结构体中的成员na和tostore加l 。
static void listfield (LexState *ls, struct ConsControl *cc) {
  /* listfield -> exp */
  // 解析表达式，放入cc-v中
  expr(ls, &cc->v);
  // 检验列表的元素是否超限
  checklimit(ls->fs, cc->na, MAX_INT, "items in a constructor");
  // 增加列表的元素个数
  cc->na++;
  // 增加有待存储的元素个数
  cc->tostore++;
}

// 
// 针对具体的字段类型来做解析，主要有如下几种类型。
// 如果解析到一个变量，那么看紧跟着这个符号的是不是＝，如果不是，就是一个数组
// 方式的赋值，否则就是散列方式的赋值。
static void field (LexState *ls, struct ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    // 变量名，可能是数组部分，也可能是散列部分
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      // 预读下一个token，
      // 不是赋值
      if (luaX_lookahead(ls) != '=')  /* expression? */
        // 加入数组部分
        listfield(ls, cc);
      else
        // 加入散列部分
        recfield(ls, cc);
      break;
    }
    // 如果看到的是［符号，就认为这是一个hash部分的构造
    case '[': {
      recfield(ls, cc);
      break;
    }
    // 默认认为就是数组部分的构造
    default: {
      listfield(ls, cc);
      break;
    }
  }
}

// 构造表的操作
static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  // 生成一条OP_NEWTABLE指令。注意，在前面关于这个指令的说明中，这条指令
  // 创建的表最终会根据指令中的参数A存储的寄存器地址，赋值给本函数栈内的寄存器，
  // 所以很显然这条指令是需要重定向的，所以就要下面VRELOCABLE的重定向的语句
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  struct ConsControl cc;
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  // 初始化重定位
  init_exp(t, VRELOCABLE, pc);
  // 将ConsControl结构体中的对象v初始化为VVOID
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  // 将寄存器地址修正为前面创建的OP_NEWTABLE指令的参数A 。
  luaK_exp2nextreg(ls->fs, t);  /* fix it at stack top */
  // 如果是{,跳过{，不是就报错
  checknext(ls, '{');
  // 遍历表中的部分
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    // 如果是结束行，跳出
    if (ls->t.token == '}') break;
	// 调用closelistfield函数生成上一个表达式的相关指令。容易想到，这肯定会
	// 调用luaK_exp2nextreg函数。注意上面提到过，最开始初始化ConsControl表达式时，其
	// 成员变量v的表达式类型是VVOID ，因此这种情况下进入这个函数并不会有什么效果，这
	// 就把循环和前面的初始化语句衔接在了一起。
    closelistfield(fs, &cc);
    // 解析字段，得到字段对应的表达式
    field(ls, &cc);
    // 找下一个字段
  } while (testnext(ls, ',') || testnext(ls, ';'));
  // 检查后面是否就是}
  check_match(ls, '}', '{', line);
  // 如果还有没写入的，将剩下的部分写入 
  lastlistfield(fs, &cc);
  // 将ConsControl结构体中存放的散列和数组部分的大小，写入前面生成的
  // OP_NEWTABLE指令的B和C部分。
  SETARG_B(fs->f->code[pc], luaO_int2fb(cc.na)); /* set initial array size */
  SETARG_C(fs->f->code[pc], luaO_int2fb(cc.nh));  /* set initial table size */
}

/* }====================================================================== */


// 分析参数列表
static void parlist (LexState *ls) {
  /* parlist -> [ param { ',' param } ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  f->is_vararg = 0;
  // 参数列表不为空
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        // 参数名
        case TK_NAME: {  /* param -> NAME */
          // 检查参数名，然后新建一个局部变量
          new_localvar(ls, str_checkname(ls));
          // 记录有变量名的变量的数目
          nparams++;
          break;
        }
        // 参数中有 ...表示可变参数
        case TK_DOTS: {  /* param -> '...' */
          luaX_next(ls);
          // 设置为可变参数
          f->is_vararg = 1;  /* declared vararg */
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!f->is_vararg && testnext(ls, ','));
  }
  // 调整局部变量
  adjustlocalvars(ls, nparams);
  // 得到固定参数的个数
  f->numparams = cast_byte(fs->nactvar);
  // 给参数列表预定寄存器
  luaK_reserveregs(fs, fs->nactvar);  /* reserve register for parameters */
}

// 解析一个函数体的信息对应地在函数body中
static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  /* body ->  '(' parlist ')' block END */
  FuncState new_fs;
  BlockCnt bl;
  // 创建一个新的函数原型
  new_fs.f = addprototype(ls);
  // 记录函数开始的行号
  new_fs.f->linedefined = line;
  // 处理函数原型的初始信息
  open_func(ls, &new_fs, &bl);
  // 跳过(
  checknext(ls, '(');
  // 是否为一个方法
  if (ismethod) {
    // 创建self局部变量
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    // 将self局部变量加入到局部变量列表中
    adjustlocalvars(ls, 1);
  }
  // 分析参数列表
  parlist(ls);
  // 确认后面是)，并且跳过
  checknext(ls, ')');
  // 开始语句
  statlist(ls);
  // 函数定义结束的行号
  new_fs.f->lastlinedefined = ls->linenumber;
  // 检查后面是否是函数结尾的Token
  check_match(ls, TK_END, TK_FUNCTION, line);
  // 函数结束指令
  codeclosure(ls, e);
  // 分析完毕之后调用close_func函数，用于将最后分析的结果保存到Proto结构体中
  close_func(ls);
}

// 解析表达式列表
static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}

// 函数参数
static void funcargs (LexState *ls, expdesc *f, int line) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  // 处理函数参数
  switch (ls->t.token) {
    // 表达式列表
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      // 如果后面紧跟），表示参数列表为空
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        // 解析表达式列表
        explist(ls, &args);
        // 设置多个返回值 
        luaK_setmultret(fs, &args);
      }
      // 最后跟着一个匹配的)
      check_match(ls, ')', '(', line);
      break;
    }
    // 函数参数是一个表
    case '{': {  /* funcargs -> constructor */
      // 构造表的操作
      constructor(ls, &args);
      break;
    }
    // 函数参数是一个字符串
    case TK_STRING: {  /* funcargs -> STRING */
      // 生成字符串的字节码 
      codestring(ls, &args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  base = f->u.info;  /* base register for call */
  // 多返回值
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      // 确保表达式的值保存在下一个寄存器中
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    // 得到参数的数目
    nparams = fs->freereg - (base+1);
  }
  // 初始化表达式，生成函数调用的指令
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  // 修正当前位置关联的行号信息
  luaK_fixline(fs, line);
  fs->freereg = base+1;  /* call remove function and arguments and leaves
                            (unless changed) one result */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

// 基本表达式
static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    // (expr)方式的表达式
    case '(': {
      int line = ls->linenumber;
      // 跳过（
      luaX_next(ls);
      // 解析表达式
      expr(ls, v);
      // 是否有右边的括号）
      check_match(ls, ')', '(', line);
      // luaK_dischargevars函数为变量表达式生成估值计算的指令 
      luaK_dischargevars(ls->fs, v);
      return;
    }
    // 变量名
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
}

// 主要用来处理赋值变量名称，判断变量的类型：局部变量、全局变量、Table格式、函数等。
// 处理后缀表达式
static void suffixedexp (LexState *ls, expdesc *v) {
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  // 解析基本表达式
  primaryexp(ls, v);
  for (;;) {
    switch (ls->t.token) {
      // 表字段
      case '.': {  /* fieldsel */
        // 名字为索引取表字段
        fieldsel(ls, v);
        break;
      }
      case '[': {  /* '[' exp1 ']' */
        expdesc key;
        // 确保表的表达式的结果要么在寄存器上，要么在一个upvalue上
        luaK_exp2anyregup(fs, v);
        // 解析table中的索引([])中的表达式
        yindex(ls, &key);
        // 创建一个表达式t[k]
        luaK_indexed(fs, v, &key);
        break;
      }
      // 冒号的方式
      case ':': {  /* ':' NAME funcargs */
        expdesc key;
        // 跳过冒号
        luaX_next(ls);
        // 获取名字表达式的值
        checkname(ls, &key);
        // 名字为索引取表字段
        luaK_self(fs, v, &key);
        // 处理函数参数和调用
        funcargs(ls, v, line);
        break;
      }
      // 函数参数
      case '(': case TK_STRING: case '{': {  /* funcargs */
        // 确保函数参数在下一个寄存器里
        luaK_exp2nextreg(fs, v);
        // 处理函数参数和调用
        funcargs(ls, v, line);
        break;
      }
      default: return;
    }
  }
}

// 简单的表达式
static void simpleexp (LexState *ls, expdesc *v) {
    // 简单表达式的EBNF范式
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  switch (ls->t.token) {
      // 浮点数，正式和字符串，直接用词法解析器里得到的值
    case TK_FLT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    // 将字符串加入常量表，然后设置对应的常量表索引
    case TK_STRING: {
      codestring(ls, v, ls->t.seminfo.ts);
      break;
    }
    // nil
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    // (...)就是不定参数 
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, fs->f->is_vararg,
                      "cannot use '...' outside a vararg function");
      // 不定参数直接编码
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 1, 0));
      break;
    }
    // 构造，对一个table赋初值
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    // 处理函数
    case TK_FUNCTION: {
      luaX_next(ls);
      // 进入函数体的解析
      body(ls, v, 0, ls->linenumber);
      return;
    }
    // 默认处理后缀表达式，主要用来处理赋值变量名称，判断变量的类型：局部变量、全局变量、Table格式、函数等。
    default: {
      suffixedexp(ls, v);
      return;
    }
  }
  luaX_next(ls);
}

// 解析一元操作符
// 一元操作符的英文：unary operator
static UnOpr getunopr (int op) {
  switch (op) {
    // 取反 not
    case TK_NOT: return OPR_NOT;
    // 减去或者取负
    case '-': return OPR_MINUS;
    // 按位非
    case '~': return OPR_BNOT;
    // 取长度
    case '#': return OPR_LEN;
    // 不是一元操作符
    default: return OPR_NOUNOPR;
  }
}

// 得到二元操作符
static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case '&': return OPR_BAND;
    case '|': return OPR_BOR;
    case '~': return OPR_BXOR;
    case TK_SHL: return OPR_SHL;
    case TK_SHR: return OPR_SHR;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}

// 每一个二元操作符左边的优先级和右边的优先级
// 比如，power操作符的左右优先级是不同的，其中左边的优先级大于右边的优先级。比如，
// 表达式2^1^2 是右结合的， 即与表达式2^(1^2)一样。
static const struct {
  // 每一个二元操作符左边的优先级
  lu_byte left;  /* left priority for each binary operator */
  // 右边优先级
  lu_byte right; /* right priority */
  // 操作符的优先级
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}            /* and, or */
};

// 一元运算符的优先级
#define UNARY_PRIORITY	12  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/ 
// subexpr -> (simpleexp | unop subexpr) { binop subexpr }
// 其中“binop”是优先级高于“limit”的任何二元运算符
// 1.在传人函数的参数中，其中有一个参数用于表示当前处理的表达式的优先级，后面将根
// 据这个参数来判断在处理二元操作符时，是先处理二元操作符左边还是右边的式子。首
// 次调用函数时，这个参数为0 ，也就是最小的优先级。
// 2.在进入函数后，首先判断获取到的是不是一元操作符，如果是，那么递归调用函数
// subexpr，此时传人的优先级是常量UNARY_PRIORITY ； 否则调用函数simpleexp 来处理简单
// 的表达式。
// 3.接着看读到的字符是不是二元操作符，如果是并且同时满足这个二元操作符的优先级大
// 于当前subexpr函数的优先级，那么递归调用函数subexpr来处理二元操作符左边的式子。
// 1。在传人函数的参数中，其中有一个参数用于表示当前处理的表达式的优先级，后面将根
// 据这个参数来判断在处理二元操作符时，是先处理二元操作符左边还是右边的式子。首
// 次调用函数时，这个参数为0 ，也就是最小的优先级。
// 2。在进入函数后，首先判断获取到的是不是一元操作符，如果是，那么递归调用函数
// subexpr，此时传人的优先级是常量UNARY_PRIORITY ； 否则调用函数simpleexp来处理简单
// 的表达式。
// 3。接着看读到的字符是不是二元操作符，如果是并且同时满足这个二元操作符的优先级大
// 于当前subexpr函数的优先级，那么递归调用函数subexpr来处理二元操作符左边的式子。

// 解析表达式
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop;
  // 进行C函数调用
  enterlevel(ls);
  // 解析一元操作符
  uop = getunopr(ls->t.token);
  // 一元操作符
  if (uop != OPR_NOUNOPR) {
    int line = ls->linenumber;
    luaX_next(ls);
    // 一元操作符的优先级进行递归调用
    subexpr(ls, v, UNARY_PRIORITY);
    // 应用一元表达式
    luaK_prefix(ls->fs, uop, v, line);
  }
  // 简单的表达式
  else simpleexp(ls, v);
  /* expand while operators have priorities higher than 'limit' */
  // 当操作符的优先级比'limit'高就展开
  op = getbinopr(ls->t.token);
  // 二元操作符，并且优先级大于limit
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);
    // 在读取第二个操作数之前处理二元运算op的第一个操作数v
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    // 读取优先级更高的子表达式到v2
    nextop = subexpr(ls, &v2, priority[op].right);
    // 处理最后的结果
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  // 退出C函数调用
  leavelevel(ls);
  return op;  /* return first untreated operator */
}

// 解析表达式
static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

// 代码块处理
static void block (LexState *ls) {
  /* block -> statlist */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  // 进入代码块
  enterblock(fs, &bl, 0);
  // 语句列表处理
  statlist(ls);
  // 离开代码块
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  int extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    if (lh->v.k == VINDEXED) {  /* assigning to a table? */
      /* table is the upvalue/local being assigned now? */
      if (lh->v.u.ind.vt == v->k && lh->v.u.ind.t == v->u.info) {
        conflict = 1;
        lh->v.u.ind.vt = VLOCAL;
        lh->v.u.ind.t = extra;  /* previous assignment will use safe copy */
      }
      /* index is the local being assigned? (index cannot be upvalue) */
      if (v->k == VLOCAL && lh->v.u.ind.idx == v->u.info) {
        conflict = 1;
        lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    OpCode op = (v->k == VLOCAL) ? OP_MOVE : OP_GETUPVAL;
    luaK_codeABC(fs, op, extra, v->u.info, 0);
    luaK_reserveregs(fs, 1);
  }
}


static void assignment (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  if (testnext(ls, ',')) {  /* assignment -> ',' suffixedexp assignment */
    struct LHS_assign nv;
    nv.prev = lh;
    suffixedexp(ls, &nv.v);
    if (nv.v.k != VINDEXED)
      check_conflict(ls, lh, &nv.v);
    checklimit(ls->fs, nvars + ls->L->nCcalls, LUAI_MAXCCALLS,
                    "C levels");
    assignment(ls, &nv, nvars+1);
  }
  else {  /* assignment -> '=' explist */
    int nexps;
    checknext(ls, '=');
    nexps = explist(ls, &e);
    if (nexps != nvars)
      adjust_assign(ls, nvars, nexps, &e);
    else {
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  luaK_storevar(ls->fs, &lh->v, &e);
}

// 解析条件表达式
static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  // 分析条件表达式
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  // 如果条件为true就继续，否则跳转 
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}

// goto语句处理
static void gotostat (LexState *ls, int pc) {
  int line = ls->linenumber;
  TString *label;
  int g;
  // 如果是goto
  if (testnext(ls, TK_GOTO))
    // 得到要跳转的标签
    label = str_checkname(ls);
  else {
    // 跳过break
    luaX_next(ls);  /* skip break */
    // 生成break标签
    label = luaS_new(ls->L, "break");
  }
  // 创建一个标签入口，加入到未处理列表中
  g = newlabelentry(ls, &ls->dyd->gt, label, line, pc);
  // 如果标签已经定义则关闭它
  findlabel(ls, g);  /* close it if label already defined */
}


/* check for repeated labels on the same block */
static void checkrepeated (FuncState *fs, Labellist *ll, TString *label) {
  int i;
  for (i = fs->bl->firstlabel; i < ll->n; i++) {
    if (eqstr(label, ll->arr[i].name)) {
      const char *msg = luaO_pushfstring(fs->ls->L,
                          "label '%s' already defined on line %d",
                          getstr(label), ll->arr[i].line);
      semerror(fs->ls, msg);
    }
  }
}


/* skip no-op statements */
static void skipnoopstat (LexState *ls) {
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    statement(ls);
}


static void labelstat (LexState *ls, TString *label, int line) {
  /* label -> '::' NAME '::' */
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  int l;  /* index of new label being created */
  checkrepeated(fs, ll, label);  /* check for repeated labels */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  /* create new entry for this label */
  l = newlabelentry(ls, ll, label, line, luaK_getlabel(fs));
  skipnoopstat(ls);  /* skip other no-op statements */
  if (block_follow(ls, 0)) {  /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
  findgotos(ls, &ll->arr[l]);
}

// while语句处理
static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  // 跳过while
  luaX_next(ls);  /* skip WHILE */
  // while开始的指令地址
  whileinit = luaK_getlabel(fs);
  // 解析条件表达式，返回不满足条件的跳转指令地址 
  condexit = cond(ls);
  // 进入代码块
  enterblock(fs, &bl, 1);
  // 检查下一步token是否为do
  checknext(ls, TK_DO);
  // 代码块处理 
  block(ls);
  // 条件跳转到while开始处的指令
  luaK_jumpto(fs, whileinit);
  // while代码块的结束
  check_match(ls, TK_END, TK_WHILE, line);
  // 离开代码块
  leaveblock(fs);
  // 填充跳转到不满足条件，跳出循环的指令
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}

// repeat语句处理
static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  // 循环起始处
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  // 进入循环代码块
  enterblock(fs, &bl1, 1);  /* loop block */
  // 进入范围块
  enterblock(fs, &bl2, 0);  /* scope block */
  // 跳过repeat语句
  luaX_next(ls);  /* skip REPEAT */
  // 处理语句列表
  statlist(ls);
  // 检查repeate对应的until
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  // 读取条件并解析条件表达式
  condexit = cond(ls);  /* read condition (inside scope block) */
  // 循环体里涉及到upvalues
  if (bl2.upval)  /* upvalues? */
    // 关闭upvalues
    luaK_patchclose(fs, condexit, bl2.nactvar);
  // 离开范围代码块
  leaveblock(fs);  /* finish scope */
  // 将循环开始地址回填条件跳转
  luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
  // 离开循环代码块
  leaveblock(fs);  /* finish loop */
}

// 解析表达式的值，将结果放入寄存器中，返回寄存器索引
static int exp1 (LexState *ls) {
  expdesc e;
  int reg;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  lua_assert(e.k == VNONRELOC);
  reg = e.u.info;
  return reg;
}

// for循环的循环体
static void forbody (LexState *ls, int base, int line, int nvars, int isnum) {
  /* forbody -> DO block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  // 控制变量，3个
  adjustlocalvars(ls, 3);  /* control variables */
  // 找到 do 保留字
  checknext(ls, TK_DO);
  // 根据是数值控制循环还是泛型循环生成不同的指令
  prep = isnum ? luaK_codeAsBx(fs, OP_FORPREP, base, NO_JUMP) : luaK_jump(fs);
  // 进入声明变量的作用域代码块
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  // 调整局部变量的数量
  adjustlocalvars(ls, nvars);
  // 将其放置在寄存器上
  luaK_reserveregs(fs, nvars);
  // 处理循环体代码块
  block(ls);
  // 离开作用域代码块
  leaveblock(fs);  /* end of scope for declared variables */
  // 回填跳转到当前的跳转列表
  luaK_patchtohere(fs, prep);
  // 数字的for
  if (isnum)  /* numeric for? */
    endfor = luaK_codeAsBx(fs, OP_FORLOOP, base, NO_JUMP);
  else {  /* generic for */
      // 泛型for
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    // 修正当前位置关联的行号信息
    luaK_fixline(fs, line);
    endfor = luaK_codeAsBx(fs, OP_TFORLOOP, base + 2, NO_JUMP);
  }
  // 回填循环结束的跳转
  luaK_patchlist(fs, endfor, prep + 1);
  // 修正当前位置关联的行号信息
  luaK_fixline(fs, line);
}

// 数字类for循环指令的处理代码
static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  // 创建三个局部变量：循环子，循环条件限制，和步长
  new_localvarliteral(ls, "(for index)");
  new_localvarliteral(ls, "(for limit)");
  new_localvarliteral(ls, "(for step)");
  // 新的局部变量
  new_localvar(ls, varname);
  checknext(ls, '=');
  // 解析初始化表达式的值，将结果放入寄存器中，返回寄存器索引
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  // 解析限制表达式的值
  exp1(ls);  /* limit */
  // 如果有步长得到步长
  if (testnext(ls, ','))
    // 解析步长表达式的值
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
      // 默认步长为1
    luaK_codek(fs, fs->freereg, luaK_intK(fs, 1));
    luaK_reserveregs(fs, 1);
  }
  // 循环体
  forbody(ls, base, line, 1, 1);
}

// 泛型循环的处理函数在函数for list中：
static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  // 生成器，状态和控制，
  int nvars = 4;  /* gen, state, control, plus at least one declared var */
  int line;
  int base = fs->freereg;
  /* create control variables */
  // 创建控制变量
  new_localvarliteral(ls, "(for generator)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for control)");
  /* create declared variables */
  // 创建声明的变量
  new_localvar(ls, indexname);
  // 解析列表
  while (testnext(ls, ',')) {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  // 找到in
  checknext(ls, TK_IN);
  line = ls->linenumber;
  // 用于根据等号两边变量和表达式的数量来调整赋值
  adjust_assign(ls, 3, explist(ls, &e), &e);
  // 用来调用发生器的额外的空间
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  // for循环体
  forbody(ls, base, line, nvars - 3, 0);
}

// 处理for循环，只要程序解析到关键字for就会进入这个函数
static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  // 进入for块
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  // 跳过for关键字
  luaX_next(ls);  /* skip 'for' */
  // 得到第一个变量名
  varname = str_checkname(ls);  /* first variable name */
  // 处理不同的for表达式方式
  switch (ls->t.token) {
    // for的数字型的处理
    case '=': fornum(ls, varname, line); break;
    // for的列表型的处理
    case ',': case TK_IN: forlist(ls, varname); break;
    // 其他情况报错
    default: luaX_syntaxerror(ls, "'=' or 'in' expected");
  }
  // 是否匹配for的结尾部分
  check_match(ls, TK_END, TK_FOR, line);
  // 离开for循环块
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}

// 处理IF cond THEN block或者ELSEIF cond THEN block这两种语句
static void test_then_block (LexState *ls, int *escapelist) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  expdesc v;
  int jf;  /* instruction to skip 'then' code (if condition is false) */
  // 跳过if或者elseif
  luaX_next(ls);  /* skip IF or ELSEIF */
  // 读取条件
  expr(ls, &v);  /* read condition */
  // 确定后面是否是then，并取下一个
  checknext(ls, TK_THEN);
  // 如果是跳转
  if (ls->t.token == TK_GOTO || ls->t.token == TK_BREAK) {
    // 如果表达式为true，则跳转，否则顺序执行
    luaK_goiffalse(ls->fs, &v);  /* will jump to label if condition is true */
    // 跳转前初始化进入的代码块数据结构
    enterblock(fs, &bl, 0);  /* must enter block before 'goto' */
    // goto语句处理
    gotostat(ls, v.t);  /* handle goto/break */
    // 跳过后面的;
    while (testnext(ls, ';')) {}  /* skip colons */
    // 检查当前的Token是否关闭当前的语句块
    if (block_follow(ls, 0)) {  /* 'goto' is the entire block? */
      // 如果是关闭的token，就离开当前的代码块
      leaveblock(fs);
      return;  /* and that is it */
    }
    // 如果条件为假，则必须跳过 'then' 部分
    else  /* must skip over 'then' part if condition is false */
      // 创建一个跳转指令并返回它的位置
      jf = luaK_jump(fs);
  }
  // 常规情况（不是goto或者break)
  else {  /* regular case (not goto/break) */
    // 如果条件为false，就跳过代码块
    luaK_goiftrue(ls->fs, &v);  /* skip over block if condition is false */
    // 初始化进入代码块
    enterblock(fs, &bl, 0);
    // 进入false的代码块
    jf = v.f;
  }
  // 分析语句列表
  statlist(ls);  /* 'then' part */
  // 离开当前代码块
  leaveblock(fs);
  // 如果是else或者elseif，跳过它
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
    luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
  // 将当前的位置加入false跳转列表中
  luaK_patchtohere(fs, jf);
}

// if语句解析 
static void ifstat (LexState *ls, int line) {
  // if语句的EBNF语法如下
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  int escapelist = NO_JUMP;  /* exit list for finished parts */
  test_then_block(ls, &escapelist);  /* IF cond THEN block */
  // elseif部分
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist);  /* ELSEIF cond THEN block */
  // else部分 
  if (testnext(ls, TK_ELSE))
    // else部分的代码块处理
    block(ls);  /* 'else' part */
  // 检查结束部分
  check_match(ls, TK_END, TK_IF, line);
  // 用当前去填充将跳转到最外面的部分
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}

// 局部函数
static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  // 新的局部变量
  new_localvar(ls, str_checkname(ls));  /* new local variable */
  // 进入其范围
  adjustlocalvars(ls, 1);  /* enter its scope */
  // 解析函数体，将其放入下一个寄存器中
  body(ls, &b, 0, ls->linenumber);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  // 设置局部变量开始的指令地址，调试信息将仅在该点之后看到变量
  getlocvar(fs, b.u.info)->startpc = fs->pc;
}

// 局部变量
static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME {',' NAME} ['=' explist] */
  int nvars = 0;
  int nexps;
  expdesc e;
  do {
    // 取得变量名，新建一个局部变量
    new_localvar(ls, str_checkname(ls));
    nvars++;
  } while (testnext(ls, ','));
  // 后面是否有初始化语句
  if (testnext(ls, '='))
.   // 解析初始化语句
    nexps = explist(ls, &e);
  else {
    e.k = VVOID;
    nexps = 0;
  }
  // 用于根据等号两边变量和表达式的数量来调整赋值。具体来说，
  // 在上面这个例子中，当变量数量多于等号右边的表达式数量时，会将多余的变量置为NIL
  adjust_assign(ls, nvars, nexps, &e);
  // 会根据变量的数量调整FuncState结构体中记录局部变量数量的
  // nactvar对象，并记录这些局部变量的startpc值。
  adjustlocalvars(ls, nvars);
}

// 得到函数名
static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  // 这才是方法的引用方式
  if (ls->t.token == ':') {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}

// 处理函数的定义，即如何把函数体信息和变量结合在一起
static void funcstat(LexState* ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  // 定义存放表达式信息的变量V和b，其中V用来保存函数名信息，b用来保存函数体信息。
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  // 得到函数名，保存结果到变量v中
  ismethod = funcname(ls, &v);
  // 解析函数体，并将返回的信息存放在b中
  body(ls, &b, ismethod, line);
  // 将前面解析出来的body信息与函数名v对应上。
  luaK_storevar(ls->fs, &v, &b);
  // 定义“发生”在第一行
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}


static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    v.prev = NULL;
    assignment(ls, &v, 1);
  }
  else {  /* stat -> func */
    check_condition(ls, v.v.k == VCALL, "syntax error");
    SETARG_C(getinstruction(fs, &v.v), 1);  /* call statement uses no results */
  }
}


static void retstat (LexState *ls) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  expdesc e;
  int first, nret;  /* registers with returned values */
  if (block_follow(ls, 1) || ls->t.token == ';')
    first = nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == fs->nactvar);
      }
      first = fs->nactvar;
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);
      else {
        luaK_exp2nextreg(fs, &e);  /* values must go to the stack */
        first = fs->nactvar;  /* return all active values */
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';');  /* skip optional semicolon */
}

// 分析语句
static void statement (LexState *ls) {
  // 得到当前的行号
  int line = ls->linenumber;  /* may be needed for error messages */
  //  进入C函数调用，增加调用栈中调用C函数的深度
  enterlevel(ls);
  // 根据词法分析器返回的token
  switch (ls->t.token) {
     // lua对单独的分号啥也不做，空语句
    case ';': {  /* stat -> ';' (empty statement) */
      luaX_next(ls);  /* skip ';' */
      break;
    }
    // if语句
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      break;
    }
    // while语句处理
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      break;
    }
    // do end语句块
    case TK_DO: {  /* stat -> DO block END */
      // 跳过do
      luaX_next(ls);  /* skip DO */
      // 处理语句块
      block(ls);
      // 结束的end
      check_match(ls, TK_END, TK_DO, line);
      break;
    }
    // for语句处理
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      break;
    }
    // repeat语句处理
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      break;
    }
    // 处理函数定义
    case TK_FUNCTION: {  /* stat -> funcstat */
      funcstat(ls, line);
      break;
    }
    // 局部变量或者局部函数
    case TK_LOCAL: {  /* stat -> localstat */
      luaX_next(ls);  /* skip LOCAL */
      // 局部函数
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls);
      // 局部变量
      else
        localstat(ls);
      break;
    }
    // 
    case TK_DBCOLON: {  /* stat -> label */
      luaX_next(ls);  /* skip double colon */
      labelstat(ls, str_checkname(ls), line);
      break;
    }
    case TK_RETURN: {  /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      retstat(ls);
      break;
    }
    case TK_BREAK:   /* stat -> breakstat */
    case TK_GOTO: {  /* stat -> 'goto' NAME */
      gotostat(ls, luaK_jump(ls->fs));
      break;
    }
    default: {  /* stat -> func | assignment */
      exprstat(ls);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= ls->fs->nactvar);
  ls->fs->freereg = ls->fs->nactvar;  /* free registers */
  // 离开C函数调用，减少C函数调用深度
  leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
// 编译main函数，它是一个普通的vararg函数，带有名为LUA_ENV的upvalue
static void mainfunc (LexState *ls, FuncState *fs) {
  BlockCnt bl;
  expdesc v;
  // 
  open_func(ls, fs, &bl);
  // 主函数总是被定义位可变参数的函数
  fs->f->is_vararg = 1;  /* main function is always declared vararg */
  init_exp(&v, VLOCAL, 0);  /* create and... */
  newupvalue(fs, ls->envn, &v);  /* ...set environment upvalue */
  // 读取第一个词法Token
  luaX_next(ls);  /* read first token */
  // 分析main函数体
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}

// 函数luaY_parser是整个Lua分析的人口函数，这个函数的返回结果就是一个Proto指针
// Lua的闭包就是Lua语法分析之后的最终产物
LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  // 创建一个新的Lua闭包
  LClosure *cl = luaF_newLclosure(L, 1);  /* create main closure */
  // 将cl设置在栈顶
  setclLvalue(L, L->top, cl);  /* anchor it (to avoid being collected) */
  luaD_inctop(L);
  // 给语法分析扫描器创建一个表
  lexstate.h = luaH_new(L);  /* create table for scanner */
  sethvalue(L, L->top, lexstate.h);  /* anchor it */
  luaD_inctop(L);
  // 新的Proto
  funcstate.f = cl->p = luaF_newproto(L);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  lua_assert(iswhite(funcstate.f));  /* do not need barrier here */
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  L->top--;  /* remove scanner's table */
  return cl;  /* closure is on the stack, too */
}

