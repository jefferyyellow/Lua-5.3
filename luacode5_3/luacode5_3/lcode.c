/*
** $Id: lcode.c,v 2.112.1.1 2017/04/19 17:20:42 roberto Exp $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#define lcode_c
#define LUA_CORE

#include "lprefix.h"


#include <math.h>
#include <stdlib.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/* Maximum number of registers in a Lua function (must fit in 8 bits) */
// lua函数最大的寄存器数目
#define MAXREGS		255

// hasjumps函数判断表达式e的truelist和falselist的数量不一致
#define hasjumps(e)	((e)->t != (e)->f)


/*
** If expression is a numeric constant, fills 'v' with its value
** and returns 1. Otherwise, returns 0.
*/
// 如果表达式为数字常量，用表达式的值填充v，并且返回1，或者返回0
static int tonumeral(const expdesc *e, TValue *v) {
    // 不是数字
    if (hasjumps(e))
    return 0;  /* not a numeral */
   // 根据整数和浮点数，进行赋值
  switch (e->k) {
    case VKINT:
      if (v) setivalue(v, e->u.ival);
      return 1;
    case VKFLT:
      if (v) setfltvalue(v, e->u.nval);
      return 1;
    default: return 0;
  }
}


/*
** Create a OP_LOADNIL instruction, but try to optimize: if the previous
** instruction is also OP_LOADNIL and ranges are compatible, adjust
** range of previous instruction instead of emitting a new one. (For
** instance, 'local a; local b' will generate a single opcode.)
*/
void luaK_nil (FuncState *fs, int from, int n) {
  Instruction *previous;
  int l = from + n - 1;  /* last register to set nil */
  if (fs->pc > fs->lasttarget) {  /* no jumps to current position? */
    previous = &fs->f->code[fs->pc-1];
    if (GET_OPCODE(*previous) == OP_LOADNIL) {  /* previous is LOADNIL? */
      int pfrom = GETARG_A(*previous);  /* get previous range */
      int pl = pfrom + GETARG_B(*previous);
      if ((pfrom <= from && from <= pl + 1) ||
          (from <= pfrom && pfrom <= l + 1)) {  /* can connect both? */
        if (pfrom < from) from = pfrom;  /* from = min(from, pfrom) */
        if (pl > l) l = pl;  /* l = max(l, pl) */
        SETARG_A(*previous, from);
        SETARG_B(*previous, l - from);
        return;
      }
    }  /* else go through */
  }
  luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0);  /* else no optimization */
}


/*
** Gets the destination address of a jump instruction. Used to traverse
** a list of jumps.
*/
// 得到跳转指令的目标地址。用于遍历跳转指令列表
static int getjump (FuncState *fs, int pc) {
  // 得到指令中sBx的值,
  int offset = GETARG_sBx(fs->f->code[pc]);
  if (offset == NO_JUMP)  /* point to itself represents end of list */
    return NO_JUMP;  /* end of list */
  else
    // 将一个偏移转换成一个绝对位置
    return (pc+1)+offset;  /* turn offset into absolute position */
}


/*
** Fix jump instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua)
*/
// 修复了位置 'pc' 处的跳转指令以跳转到 'dest'。
// lua中跳转地址都是相对的
static void fixjump (FuncState *fs, int pc, int dest) {
  // 得到pc对应的指令
  Instruction *jmp = &fs->f->code[pc];
  // 计算连接的地址到当前地址的相对位置
  int offset = dest - (pc + 1);
  lua_assert(dest != NO_JUMP);
  // 跳转位置进行校验
  if (abs(offset) > MAXARG_sBx)
    luaX_syntaxerror(fs->ls, "control structure too long");
  // 设置跳转
  SETARG_sBx(*jmp, offset);
}


/*
** Concatenate jump-list 'l2' into jump-list 'l1'
*/
// 将跳转列表l2连接到跳转列表l1中
// 将一个新的跳转位置加入空悬跳转链表的操作
// 可以看到，这个跳转链表的实现并不像经典的链表实现那样，有一个类似next 的指针指向下
// 一个元素，而是利用了跳转指令中的跳转地址这一个参数来存储链表中下一个元素的值。
void luaK_concat (FuncState *fs, int *l1, int l2) {
    // l2不是个跳转列表
  if (l2 == NO_JUMP) return;  /* nothing to concatenate? */
  // l1本身为空，那就直接赋值
  else if (*l1 == NO_JUMP)  /* no original list? */
    *l1 = l2;  /* 'l1' points to 'l2' */
  else {
    int list = *l1;
    int next;
    // 找到最后一个元素
    while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
      list = next;
    // 将l2链接在最后
    fixjump(fs, list, l2);  /* last element links to 'l2' */
  }
}


/*
** Create a jump instruction and return its position, so its destination
** can be fixed later (with 'fixjump'). If there are jumps to
** this position (kept in 'jpc'), link them all together so that
** 'patchlistaux' will fix all them directly to the final destination.
*/
// 创建一个跳转指令并返回它的位置，然后它的目标地址可以稍后修复（使用'fixjump'）。
// 如果有跳转到这个位置（保存在“jpc”中），将它们链接在一起，以便调用'patchlistaux'
// 将最终的目的地址填写在里面
int luaK_jump (FuncState *fs) {
  int jpc = fs->jpc;  /* save list of jumps to here */
  int j;
  fs->jpc = NO_JUMP;  /* no more jumps to here */
  // 生成jmp指令
  j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
  // 最后将前面预存的jpc指针加入到新生成的OP_JMP指令的跳转位置中
  luaK_concat(fs, &j, jpc);  /* keep them on hold */
  return j;
}


/*
** Code a 'return' instruction
*/
// 编码一个返回指令
void luaK_ret (FuncState *fs, int first, int nret) {
  luaK_codeABC(fs, OP_RETURN, first, nret+1, 0);
}


/*
** Code a "conditional jump", that is, a test or comparison opcode
** followed by a jump. Return jump position.
*/
// 生成“条件跳转”代码，即测试或比较操作码后跟跳转。 返回跳跃位置。
static int condjump (FuncState *fs, OpCode op, int A, int B, int C) {
  // 测试比较操作字节码
  luaK_codeABC(fs, op, A, B, C);
  // 跳转字节码
  return luaK_jump(fs);
}


/*
** returns current 'pc' and marks it as a jump target (to avoid wrong
** optimizations with consecutive instructions not in the same basic block).
*/
// 返回当前的 'pc' 并将其标记为跳转目标（以避免错误
// 使用不在同一基本块中的连续指令进行优化）。
int luaK_getlabel (FuncState *fs) {
  fs->lasttarget = fs->pc;
  return fs->pc;
}


/*
** Returns the position of the instruction "controlling" a given
** jump (that is, its condition), or the jump itself if it is
** unconditional.
*/
// 返回“控制”给定跳转的指令的位置（即它的条件），如果它是无条件的，则返回跳转本身。 
// 就是找到跳转指令的条件判断指令的位置，如果是条件跳转，就返回条件判断所在的位置
static Instruction *getjumpcontrol (FuncState *fs, int pc) {
  Instruction *pi = &fs->f->code[pc];
  // 条件判断
  if (pc >= 1 && testTMode(GET_OPCODE(*(pi-1))))
    // 条件判断就是前一条指令
    return pi-1;
  else
    // 无条件判断就是当前指令
    return pi;
}


/*
** Patch destination register for a TESTSET instruction.
** If instruction in position 'node' is not a TESTSET, return 0 ("fails").
** Otherwise, if 'reg' is not 'NO_REG', set it as the destination
** register. Otherwise, change instruction to a simple 'TEST' (produces
** no register value)
*/
// 修正TESTSET指令的目标寄存器。
// 如果位置“节点”的指令不是TESTSET，则返回 0（“fails”）。
// 否则，如果 'reg' 不是 'NO_REG'，则将其设置为目标寄存器。 
// 否则，将指令更改为简单的“TEST”（不产生寄存器值）
static int patchtestreg (FuncState *fs, int node, int reg) {
  Instruction *i = getjumpcontrol(fs, node);
  // 在跳转指令不是紧跟在OP_TESTSET指令后面的情况下， patchtestreg 返回0
  // 这里的reg是需要赋值的目的寄存器地址，也就是OP_TESTSET指令中的参数A ，
  // 当这个值有效并且不等于参数B时，直接使用这个值赋值给OP_TESTSET指令的参数A 。
  if (GET_OPCODE(*i) != OP_TESTSET)
    return 0;  /* cannot patch other instructions */
  // 如果reg不是非法的寄存器，且不等于R(B) 
  if (reg != NO_REG && reg != GETARG_B(*i))
    SETARG_A(*i, reg);
  else {
     /* no register to put value or register already has the value;
        change instruction to simple test */
	// 就是没有寄存器进行赋值，或者寄存器中已经存在值（参数A与参数B 相等的情况下），
	// 此时将原先的OP_TESTSET指令修改为OP TEST指令
    *i = CREATE_ABC(OP_TEST, GETARG_B(*i), 0, GETARG_C(*i));
  }
  return 1;
}


/*
** Traverse a list of tests ensuring no one produces a value
*/
// 遍历一系列测试确保没有使用寄存器值
static void removevalues (FuncState *fs, int list) {
  // 遍历跳转表
  for (; list != NO_JUMP; list = getjump(fs, list))
      // 将比较指令都设置为不使用寄存器值
      patchtestreg(fs, list, NO_REG);
}


/*
** Traverse a list of tests, patching their destination address and
** registers: tests producing values jump to 'vtarget' (and put their
** values in 'reg'), other tests jump to 'dtarget'.
*/
// 遍历所有的列表，修复他们的目标地址和寄存器：
// tests跳转到'vtarget'的值（并将它们的值放入'reg'中），否则tests调整到'dtarget'
// patchlistaux 函数中的vtarget 指的是value target ，表示此时所需的非布尔类型值已经
// 在reg 寄存器中，此时只需要使用final ，也就是表达式的下一个指令地址对跳转地址进行回填；
static void patchlistaux (FuncState *fs, int list, int vtarget, int reg,
                          int dtarget) {
    // 遍历跳转列表
  while (list != NO_JUMP) {
      // 先暂时保存下一个跳转列表项
    int next = getjump(fs, list);
    // 回填地址
	// 如果传人的跳转指令是紧跟在OP_TESTSET指令的，就返回l
    if (patchtestreg(fs, list, reg))
      fixjump(fs, list, vtarget);
    else
      // 跳转到默认的目标
      fixjump(fs, list, dtarget);  /* jump to default target */
    list = next;
  }
}


/*
** Ensure all pending jumps to current position are fixed (jumping
** to current position with no values) and reset list of pending
** jumps
*/
// 确保所有悬空的跳转到当前位置的指令目标地址都回填好（跳转到没有值的当前位置）
// 重置悬空的跳转列表
static void dischargejpc (FuncState *fs) {
  patchlistaux(fs, fs->jpc, fs->pc, NO_REG, fs->pc);
  fs->jpc = NO_JUMP;
}


/*
** Add elements in 'list' to list of pending jumps to "here"
** (current position)
*/
// 将“list”中的元素添加到“here”的挂起跳转列表(当前位置)
// FuncState结构体有一个名为jpc的成员，它将需要回填为下一个待生成指令地址的跳转指令
// 链接到一起。这个操作是在luaK_patchtohere函数中进行的：
void luaK_patchtohere (FuncState *fs, int list) {
  luaK_getlabel(fs);  /* mark "here" as a jump target */
  luaK_concat(fs, &fs->jpc, list);
}


/*
** Path all jumps in 'list' to jump to 'target'.
** (The assert means that we cannot fix a jump to a forward address
** because we only know addresses once code is generated.)
*/
// 回填跳转到‘target'的所有跳转列表的项
// 断言意味着我们无法修复到前向地址的跳转因为我们只有在生成代码后才知道地址
void luaK_patchlist (FuncState *fs, int list, int target) {
  // 如果目标是当前位置，加入跳转列表
  if (target == fs->pc)  /* 'target' is current position? */
    luaK_patchtohere(fs, list);  /* add list to pending jumps */
  else {
    lua_assert(target < fs->pc);
    // 遍历所有的列表，修复他们的目标地址和寄存器
    patchlistaux(fs, list, target, NO_REG, target);
  }
}


/*
** Path all jumps in 'list' to close upvalues up to given 'level'
** (The assertion checks that jumps either were closing nothing
** or were closing higher levels, from inner blocks.)
*/
// 将“列表”中的所有跳转路径的close upvalues限制最大为指定的level
//（断言检查跳转要么没有关闭任何内容，要么关闭更高级别，来自内部块。）
void luaK_patchclose (FuncState *fs, int list, int level) {
  // 参数+1的目的是保留0表示无操作
  level++;  /* argument is +1 to reserve 0 as non-op */
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    lua_assert(GET_OPCODE(fs->f->code[list]) == OP_JMP &&
                (GETARG_A(fs->f->code[list]) == 0 ||
                 GETARG_A(fs->f->code[list]) >= level));
    SETARG_A(fs->f->code[list], level);
  }
}


/*
** Emit instruction 'i', checking for array sizes and saving also its
** line information. Return 'i' position.
*/
// 将指令i放入指令列表中，并且保存其对应的行号信息，返回指令i所在的位置
static int luaK_code (FuncState *fs, Instruction i) {
  Proto *f = fs->f;
  // 回填所有跳转到pc的目标地址
  dischargejpc(fs);  /* 'pc' will change */
  /* put new instruction in code array */
  // 增加code的长度，确定f->code还能放得下，放不下就增长
  luaM_growvector(fs->ls->L, f->code, fs->pc, f->sizecode, Instruction,
                  MAX_INT, "opcodes");
  // 将指令放入指令列表中
  f->code[fs->pc] = i;
  // 保存对应的行号信息
  /* save corresponding line information */
  luaM_growvector(fs->ls->L, f->lineinfo, fs->pc, f->sizelineinfo, int,
                  MAX_INT, "opcodes");
  // 保存指令所在的行
  f->lineinfo[fs->pc] = fs->ls->lastline;
  return fs->pc++;
}


/*
** Format and emit an 'iABC' instruction. (Assertions check consistency
** of parameters versus opcode.)
*/
// 格式化并生成'iABC'指令集，断言检查参数与操作码的一致性
int luaK_codeABC (FuncState *fs, OpCode o, int a, int b, int c) {
  lua_assert(getOpMode(o) == iABC);
  lua_assert(getBMode(o) != OpArgN || b == 0);
  lua_assert(getCMode(o) != OpArgN || c == 0);
  lua_assert(a <= MAXARG_A && b <= MAXARG_B && c <= MAXARG_C);
  // CREATE_ABC生成对应的字节码，加入当前的指令列表中
  return luaK_code(fs, CREATE_ABC(o, a, b, c));
}


/*
** Format and emit an 'iABx' instruction.
*/
int luaK_codeABx (FuncState *fs, OpCode o, int a, unsigned int bc) {
  lua_assert(getOpMode(o) == iABx || getOpMode(o) == iAsBx);
  lua_assert(getCMode(o) == OpArgN);
  lua_assert(a <= MAXARG_A && bc <= MAXARG_Bx);
  return luaK_code(fs, CREATE_ABx(o, a, bc));
}


/*
** Emit an "extra argument" instruction (format 'iAx')
*/
// 构造“额外参数”指令（格式为“iAx”）
static int codeextraarg (FuncState *fs, int a) {
  lua_assert(a <= MAXARG_Ax);
  // 构建一个“额外参数”指令，加入指令列表
  return luaK_code(fs, CREATE_Ax(OP_EXTRAARG, a));
}


/*
** Emit a "load constant" instruction, using either 'OP_LOADK'
** (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
** instruction with "extra argument".
*/
int luaK_codek (FuncState *fs, int reg, int k) {
  if (k <= MAXARG_Bx)
    return luaK_codeABx(fs, OP_LOADK, reg, k);
  else {
    int p = luaK_codeABx(fs, OP_LOADKX, reg, 0);
    codeextraarg(fs, k);
    return p;
  }
}


/*
** Check register-stack level, keeping track of its maximum size
** in field 'maxstacksize'
*/
// 检查寄存器堆栈层级，在“maxstacksize”字段中跟踪其最大大小
void luaK_checkstack (FuncState *fs, int n) {
  int newstack = fs->freereg + n;
  if (newstack > fs->f->maxstacksize) {
    if (newstack >= MAXREGS)
      luaX_syntaxerror(fs->ls,
        "function or expression needs too many registers");
    fs->f->maxstacksize = cast_byte(newstack);
  }
}


/*
** Reserve 'n' registers in register stack
*/
// 预定寄存器 实际上是占用n个寄存器的意思
void luaK_reserveregs (FuncState *fs, int n) {
  luaK_checkstack(fs, n);
  // 将第一个空闲的寄存器后移
  fs->freereg += n;
}


/*
** Free register 'reg', if it is neither a constant index nor
** a local variable.
)
*/
// 如果它既不是一个常量索引也不是一个局部变量索引，就释放寄存器reg
static void freereg (FuncState *fs, int reg) {
    // 不是常量并且不是活动的寄存器了
  if (!ISK(reg) && reg >= fs->nactvar) {
    fs->freereg--;
    lua_assert(reg == fs->freereg);
  }
}


/*
** Free register used by expression 'e' (if any)
*/
// 释放表达式e用过的寄存器
static void freeexp (FuncState *fs, expdesc *e) {
    // 表示分配的寄存器，可以动态释放的
  if (e->k == VNONRELOC)
    freereg(fs, e->u.info);
}


/*
** Free registers used by expressions 'e1' and 'e2' (if any) in proper
** order.
*/
// 以合适的顺序释放表达式e1和e2使用过的寄存器
static void freeexps (FuncState *fs, expdesc *e1, expdesc *e2) {
  int r1 = (e1->k == VNONRELOC) ? e1->u.info : -1;
  int r2 = (e2->k == VNONRELOC) ? e2->u.info : -1;
  // 从大到小的方式释放
  if (r1 > r2) {
    freereg(fs, r1);
    freereg(fs, r2);
  }
  else {
    freereg(fs, r2);
    freereg(fs, r1);
  }
}


/*
** Add constant 'v' to prototype's list of constants (field 'k').
** Use scanner's table to cache position of constants in constant list
** and try to reuse constants. Because some values should not be used
** as keys (nil cannot be a key, integer keys can collapse with float
** keys), the caller must provide a useful 'key' for indexing the cache.
*/
// 将常量“v”添加到原型的常量列表（字段“k”）。使用扫描器的表来缓存常量列表中常量的位置，
// 并尝试重用常量。 因为某些值不应该用作键（nil 不能是键，整数键可以用浮点键折叠），
// 调用者必须提供一个有用的“键”来索引缓存。
static int addk (FuncState *fs, TValue *key, TValue *v) {
  lua_State *L = fs->ls->L;
  Proto *f = fs->f;
  // 设置键，如有对应的值，返回，没有就新建
  TValue *idx = luaH_set(L, fs->ls->h, key);  /* index scanner table */
  int k, oldsize;
  // 索引本来就存在 
  if (ttisinteger(idx)) {  /* is there an index there? */
    // 取得索引
    k = cast_int(ivalue(idx));
    /* correct value? (warning: must distinguish floats from integers!) */
    // 索引在常量数目范围内，类型和索引k的相同，并且值也相等，就重用索引
    if (k < fs->nk && ttype(&f->k[k]) == ttype(v) &&
                      luaV_rawequalobj(&f->k[k], v))
      return k;  /* reuse index */
  }
  /* constant not found; create a new entry */
  // 常量表中没有找到，就创建一个新的条目
  oldsize = f->sizek;
  // 将当前的数目当前索引
  k = fs->nk;
  /* numerical value does not need GC barrier;
     table has no metatable, so it does not need to invalidate cache */
  setivalue(idx, k);

  luaM_growvector(L, f->k, k, f->sizek, TValue, MAXARG_Ax, "constants");
  // 函数原型表的和解析中常量列表相差的条目，都设置为nil
  while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
  // 设置f[k]=v
  setobj(L, &f->k[k], v);
  // 增加数量
  fs->nk++;
  luaC_barrier(L, f, v);
  return k;
}


/*
** Add a string to list of constants and return its index.
*/
// 将字符串添加到常量列表并返回其索引
int luaK_stringK (FuncState *fs, TString *s) {
  // 取出字符串的值，设置到临时变量o中
  TValue o;
  setsvalue(fs->ls->L, &o, s);
  // 将o加入到字符串常量列表中，使用字符串本身当键
  return addk(fs, &o, &o);  /* use string itself as key */
}


/*
** Add an integer to list of constants and return its index.
** Integers use userdata as keys to avoid collision with floats with
** same value; conversion to 'void*' is used only for hashing, so there
** are no "precision" problems.
*/
// 将整数添加到常量列表并返回其索引。整数使用userdata作为键以避免与具有相同值的浮点数发生冲突；
// 转换为“void*”仅用于hash，因此不存在“精度”问题。
int luaK_intK (FuncState *fs, lua_Integer n) {
  TValue k, o;
  setpvalue(&k, cast(void*, cast(size_t, n)));
  setivalue(&o, n);
  return addk(fs, &k, &o);
}

/*
** Add a float to list of constants and return its index.
*/
// 增加一个浮点数到常量列表，并返回其索引
static int luaK_numberK (FuncState *fs, lua_Number r) {
  TValue o;
  setfltvalue(&o, r);
  return addk(fs, &o, &o);  /* use number itself as key */
}


/*
** Add a boolean to list of constants and return its index.
*/
// 加入一个boolean值到常量列表，并返回其索引
static int boolK (FuncState *fs, int b) {
  TValue o;
  setbvalue(&o, b);
  return addk(fs, &o, &o);  /* use boolean itself as key */
}


/*
** Add nil to list of constants and return its index.
*/
// 增加nil值到常量列表，并返回其索引
static int nilK (FuncState *fs) {
  TValue k, v;
  setnilvalue(&v);
  /* cannot use nil as key; instead use table itself to represent nil */
  // 不能用nil作为key，用table作为nil的key值
  sethvalue(fs->ls->L, &k, fs->ls->h);
  return addk(fs, &k, &v);
}


/*
** Fix an expression to return the number of results 'nresults'.
** Either 'e' is a multi-ret expression (function call or vararg)
** or 'nresults' is LUA_MULTRET (as any expression can satisfy that).
*/
// 修复一个表达式以返回nresults的结果。
// e是一个多返回值的表达式（函数调用或vararg）或者nresult是LUA_MULTRET（因为任何表达式都可以满足）。
void luaK_setreturns (FuncState *fs, expdesc *e, int nresults) {
  if (e->k == VCALL) {  /* expression is an open function call? */
    // 函数调用
    SETARG_C(getinstruction(fs, e), nresults + 1);
  }
  // 不定参数
  else if (e->k == VVARARG) {
    // 得到指令
    Instruction *pc = &getinstruction(fs, e);
    SETARG_B(*pc, nresults + 1);
    SETARG_A(*pc, fs->freereg);
    // 预订寄存器
    luaK_reserveregs(fs, 1);
  }
  else lua_assert(nresults == LUA_MULTRET);
}


/*
** Fix an expression to return one result.
** If expression is not a multi-ret expression (function call or
** vararg), it already returns one result, so nothing needs to be done.
** Function calls become VNONRELOC expressions (as its result comes
** fixed in the base register of the call), while vararg expressions
** become VRELOCABLE (as OP_VARARG puts its results where it wants).
** (Calls are created returning one result, so that does not need
** to be fixed.)
*/
// 修正一个表达式来返回一个值
void luaK_setoneret (FuncState *fs, expdesc *e) {
    // 函数调用
  if (e->k == VCALL) {  /* expression is an open function call? */
    /* already returns 1 value */
    lua_assert(GETARG_C(getinstruction(fs, e)) == 2);
    // 结构有固定的位置
    e->k = VNONRELOC;  /* result has fixed position */
    e->u.info = GETARG_A(getinstruction(fs, e));
  }
  // 可变参数
  else if (e->k == VVARARG) {
    SETARG_B(getinstruction(fs, e), 2);
    e->k = VRELOCABLE;  /* can relocate its simple result */
  }
}


/*
** Ensure that expression 'e' is not a variable.
*/
// luaK_dischargevars函数为变量表达式生成估值计算的指令。对于VLOCAL类型，值就存在于局部变量对应的寄存器中，
// 不需要生成任何获取指令，也不需要分配寄存器来存储临时值。VLOCAL被转化为VNONRELOC类型，代表已经为这个表达
// 式生成了指令，并且也分配了寄存器保存这个值。对于VUPVAL类型，需要产生指令OP_GETUPVAL来获取其值。
// 而对于VINDEXED类型，根据vt的不同，需要产生OP_GETTABLE或者OP_GETTABUP指令来获取其值。
// VUPVAL和VINDEXED都被转化为VRELOCABLE类型，表示获取指令已经生成，但是指令的目标寄存器(A)还没有确定，
// 等待回填。回填后，VRELOCABLE类型会转化成VNONRELOC类型
// 变量表达式除了用来获取变量值，还有另外一个用途，就是在赋值语句中当作赋值的目标，
// 也就是将其他表达式的值存储到这个变量表达式中

// 确保表达式e不是一个变量
// 当需要使用一个非寄存器变量时，需要先通过luaK_dischargevars来保证它是“万事俱备，只欠寄存器分配”，
// 在luaK_dischargevars函数中，如果变量是在upvalue中，则需要先生成从up列表中加载该变量的机器指令，
// 但是并没有分配寄存器，也即是它的结果可以放在任意寄存器中，之后该表达式的类型为VRELOCABLE
void luaK_dischargevars (FuncState *fs, expdesc *e) {
  switch (e->k) {
    // 已经在寄存器里了
	// local a = 10;如果一个变量是VLOCAL ，说明前面已经看到过这个变量了，比如这里的局部变量a ，它在第
	// 一行代码中已经出现了，那么它既不需要重定向，也不需要额外的语句把这个值加载进来的。
    case VLOCAL: {  /* already in a register */
      // 变成了一个不需要重定位的值
      e->k = VNONRELOC;  /* becomes a non-relocatable value */
      break;
    }
    // upvalue变量
    case VUPVAL: {  /* move value to some (pending) register */
      e->u.info = luaK_codeABC(fs, OP_GETUPVAL, 0, e->u.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    // 索引变量
    case VINDEXED: {
      OpCode op;
      freereg(fs, e->u.ind.idx);
      // 局部变量
      if (e->u.ind.vt == VLOCAL) {  /* is 't' in a register? */
        freereg(fs, e->u.ind.t);
        op = OP_GETTABLE;
      }
      // upvalue
      else {
        lua_assert(e->u.ind.vt == VUPVAL);
        op = OP_GETTABUP;  /* 't' is in an upvalue */
      }
      e->u.info = luaK_codeABC(fs, op, 0, e->u.ind.t, e->u.ind.idx);
      e->k = VRELOCABLE;
      break;
    }
    case VVARARG: case VCALL: {
      luaK_setoneret(fs, e);
      break;
    }
    default: break;  /* there is one value available (somewhere) */
  }
}


/*
** Ensures expression value is in register 'reg' (and therefore
** 'e' will become a non-relocatable expression).
*/
// 确保表达式值在寄存器“reg”中（因此'e' 将成为不可重定位的表达式）。
// 就是将指令涉及的值放入指定的寄存器中
static void discharge2reg (FuncState *fs, expdesc *e, int reg) {
  luaK_dischargevars(fs, e);
  // 表达式的值是常值, 这里生成指令并回填R(A) 
  switch (e->k) {
    case VNIL: {
      luaK_nil(fs, reg, 1);
      break;
    }
    case VFALSE: case VTRUE: {
      luaK_codeABC(fs, OP_LOADBOOL, reg, e->k == VTRUE, 0);
      break;
    }
    case VK: {
      luaK_codek(fs, reg, e->u.info);
      break;
    }
    case VKFLT: {
      luaK_codek(fs, reg, luaK_numberK(fs, e->u.nval));
      break;
    }
    case VKINT: {
      luaK_codek(fs, reg, luaK_intK(fs, e->u.ival));
      break;
    }
	// 当一个变量类型是重定向时，根据reg参数来写入这个指令的参数A 。在下面的代码中，就是
	// 根据传人的reg参数，也就是获取到全局变量之后存放的寄存器地址，来重新回填到
	// 指令的A参数中。
    case VRELOCABLE: {
      Instruction *pc = &getinstruction(fs, e);
      SETARG_A(*pc, reg);  /* instruction will put result in 'reg' */
      break;
    }
    // 如果一个表达式类型是VNONRELOC ，也就是不需要重定位，那么直接生成MOVE指令来完成变量的赋值。
    case VNONRELOC: {
      if (reg != e->u.info)
        luaK_codeABC(fs, OP_MOVE, reg, e->u.info, 0);
      break;
    }
    default: {
      lua_assert(e->k == VJMP);
      return;  /* nothing to do... */
    }
  }
  e->u.info = reg;
  e->k = VNONRELOC;
}


/*
** Ensures expression value is in any register.
*/
// 保证表达式的值都在寄存器上 
// 将指令表达式涉及的变量如果不在寄存器中，就放入最后一个空闲寄存器中
static void discharge2anyreg (FuncState *fs, expdesc *e) {
  // 如果还没确定寄存器
  if (e->k != VNONRELOC) {  /* no fixed register yet? */
    // 分配一个寄存器
    luaK_reserveregs(fs, 1);  /* get a register */
    // 然后将值放入寄存器中 
    discharge2reg(fs, e, fs->freereg-1);  /* put value there */
  }
}

// 加载bool并且根据其跳转和处理
static int code_loadbool (FuncState *fs, int A, int b, int jump) {
  //  返回当前的'pc'并将其标记为跳转目标
  luaK_getlabel(fs);  /* those instructions may be jump targets */
  // 加载bool并且根据其跳转和处理
  return luaK_codeABC(fs, OP_LOADBOOL, A, b, jump);
}


/*
** check whether list has any jump that do not produce a value
** or produce an inverted value
*/
// 检查列表是否有不产生任何值或产生反转值的跳转
static int need_value (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    Instruction i = *getjumpcontrol(fs, list);
    // 不是OP_TESTSET是需要值的
    if (GET_OPCODE(i) != OP_TESTSET) return 1;
  }
  return 0;  /* not found */
}


/*
** Ensures final expression result (including results from its jump
** lists) is in register 'reg'.
** If expression has jumps, need to patch these jumps either to
** its final position or to "load" instructions (for those tests
** that do not produce values).
*/
// 确保最终表达式结果（包括其跳转列表的结果）在寄存器“reg”中。如果表达式有跳转，
// 需要将这些跳转修补到其最终位置或“加载”指令（对于那些不产生值的测试）
static void exp2reg (FuncState *fs, expdesc *e, int reg) {
  // 确保表达式的值在reg寄存器中
  discharge2reg(fs, e, reg);
  // 跳转表达式
  if (e->k == VJMP)  /* expression itself is a test? */
    // 加入跳转表e-t中
    luaK_concat(fs, &e->t, e->u.info);  /* put this jump in 't' list */
  // 需要跳转
  if (hasjumps(e)) {
    int final;  /* position after whole expression */
    int p_f = NO_JUMP;  /* position of an eventual LOAD false */
    int p_t = NO_JUMP;  /* position of an eventual LOAD true */
    // 如果跳转的true列表或者false列表需要值 
    if (need_value(fs, e->t) || need_value(fs, e->f)) {
      int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
      // 加载bool并且根据其跳转和处理 
      p_f = code_loadbool(fs, reg, 0, 1);
      p_t = code_loadbool(fs, reg, 1, 0);
      // 将fj中的元素添加到fs->jpc的挂起跳转列表(当前位置)
      luaK_patchtohere(fs, fj);
    }
    final = luaK_getlabel(fs);
    // 遍历所有的true和false的跳转表，修复他们的目标地址和寄存器：
    patchlistaux(fs, e->f, final, reg, p_f);
    patchlistaux(fs, e->t, final, reg, p_t);
  }
  e->f = e->t = NO_JUMP;
  e->u.info = reg;
  e->k = VNONRELOC;
}


/*
** Ensures final expression result (including results from its jump
** lists) is in next available register.
*/
// 确保表达式的结果（包括从它的跳转表的结果）保存在下一个可用的寄存器中
void luaK_exp2nextreg (FuncState *fs, expdesc *e) {
	// 根据变量所在的不同作用域（ local, global, upvalue )来决定这个变量是否需要重定向。
  luaK_dischargevars(fs, e);
  // 释放刚用过的寄存器
  freeexp(fs, e);
  // 调用luaK_reserveregs 函数，分配可用的函数寄存器空间，得到这个空间对应的寄存器
  // 索引。有了空间，才能存储变量。
  luaK_reserveregs(fs, 1);
  // 调用exp2reg函数，真正完成把表达式的数据放入寄存器空间的工作。在这个函数中，最
  // 终又会调用discharge2reg 函数，这个函数式根据不同的表达式类型（ NIL ，布尔表达式，
  // 数字等）来生成存取表达式的值到寄存器的字节码。
  exp2reg(fs, e, fs->freereg - 1);
}


/*
** Ensures final expression result (including results from its jump
** lists) is in some (any) register and return that register.
*/
// LOAD_XXX 加载指令
// 将表达式的值加载到寄存器中(eg:VGLOBAL, VINDEXED)，已加载到reg中的则无需此步骤(VNONRELOC)),
// 确保最终表达式结果（包括其跳转列表的结果）在某个（任何）寄存器中并返回该寄存器。
int luaK_exp2anyreg (FuncState *fs, expdesc *e) {
  // 对表达式生成估值指令
  luaK_dischargevars(fs, e);
  // 表达式有一个寄存器了
  if (e->k == VNONRELOC) {  /* expression already has a register? */
    // 没用调整，直接返回寄存器
    if (!hasjumps(e))  /* no jumps? */
      return e->u.info;  /* result is already in a register */
    // 不是一个局部变量
    if (e->u.info >= fs->nactvar) {  /* reg. is not a local? */
      exp2reg(fs, e, e->u.info);  /* put final result in it */
      return e->u.info;
    }
  }
  // e的src值还不在reg则将其存入reg 
  luaK_exp2nextreg(fs, e);  /* otherwise, use next available register */
  // 返回
  return e->u.info;
}


/*
** Ensures final expression result is either in a register or in an
** upvalue.
*/
// 确保表达式的结果要么在寄存器上，要么在一个upvalue上
void luaK_exp2anyregup (FuncState *fs, expdesc *e) {
  if (e->k != VUPVAL || hasjumps(e))
    // 确保最终表达式结果加载到寄存器
    luaK_exp2anyreg(fs, e);
}


/*
** Ensures final expression result is either in a register or it is
** a constant.
*/
// 确保最终表达式结果在寄存器中或者是常量
void luaK_exp2val (FuncState *fs, expdesc *e) {
  // 是否有跳转
  if (hasjumps(e))
    // 将表达式的值加载到寄存器中(eg:VGLOBAL, VINDEXED)，
    luaK_exp2anyreg(fs, e);
  else
    // luaK_dischargevars函数为变量表达式生成估值计算的指令
    luaK_dischargevars(fs, e);
}


/*
** Ensures final expression result is in a valid R/K index
** (that is, it is either in a register or in 'k' with an index
** in the range of R/K indices).
** Returns R/K index.
*/
// 确保最终表达式结果在有效的R/K索引中（即，它在寄存器中或在索引在
// R/K索引范围内的'k'中）。返回R/K指数。
int luaK_exp2RK (FuncState *fs, expdesc *e) {
  // 确保最终表达式结果在寄存器中或者是常量
  luaK_exp2val(fs, e);
  switch (e->k) {  /* move constants to 'k' */
    // 得到常量列表中的索引
    case VTRUE: e->u.info = boolK(fs, 1); goto vk;
    case VFALSE: e->u.info = boolK(fs, 0); goto vk;
    case VNIL: e->u.info = nilK(fs); goto vk;
    case VKINT: e->u.info = luaK_intK(fs, e->u.ival); goto vk;
    case VKFLT: e->u.info = luaK_numberK(fs, e->u.nval); goto vk;
    case VK:
    vk:
      // 表达式的类型该成常量表达式
      e->k = VK;
      if (e->u.info <= MAXINDEXRK)  /* constant fits in 'argC'? */
        // 将常量索引编码为RK值 
        return RKASK(e->u.info);
      else break;
    default: break;
  }
  /* not a constant in the right range: put it in a register */
  // 不是常量，就放在寄存器中
  return luaK_exp2anyreg(fs, e);
}


/*
** Generate code to store result of expression 'ex' into variable 'var'.
*/
// 生成代码将表达式ex的结果存入变量var
void luaK_storevar (FuncState *fs, expdesc *var, expdesc *ex) {
  switch (var->k) {
    case VLOCAL: {
      freeexp(fs, ex);
      // 确保最终表达式结果在寄存器中
      exp2reg(fs, ex, var->u.info);  /* compute 'ex' into proper place */
      return;
    }
    case VUPVAL: {
      int e = luaK_exp2anyreg(fs, ex);
      luaK_codeABC(fs, OP_SETUPVAL, e, var->u.info, 0);
      break;
    }
    case VINDEXED: {
      OpCode op = (var->u.ind.vt == VLOCAL) ? OP_SETTABLE : OP_SETTABUP;
      // 得到常量或者寄存器索引，写入指令中
      int e = luaK_exp2RK(fs, ex);
      luaK_codeABC(fs, op, var->u.ind.t, var->u.ind.idx, e);
      break;
    }
    default: lua_assert(0);  /* invalid var kind to store */
  }
  // 是否表达式使用过的寄存器
  freeexp(fs, ex);
}


/*
** Emit SELF instruction (convert expression 'e' into 'e:key(e,').
*/
// 生成取self的指令（将表达式e转化成e:key(e）)
void luaK_self (FuncState *fs, expdesc *e, expdesc *key) {
  int ereg;
  luaK_exp2anyreg(fs, e);
  ereg = e->u.info;  /* register where 'e' was placed */
  freeexp(fs, e);
  // self的基础寄存器，也就是e的地址
  e->u.info = fs->freereg;  /* base register for op_self */
  e->k = VNONRELOC;  /* self expression has a fixed register */
  // 为函数和self预留寄存器
  luaK_reserveregs(fs, 2);  /* function and 'self' produced by op_self */
  // 写入字节码
  luaK_codeABC(fs, OP_SELF, e->u.info, ereg, luaK_exp2RK(fs, key));
  freeexp(fs, key);
}


/*
** Negate condition 'e' (where 'e' is a comparison).
*/
// 当e是一个条件比较，否定条件'e'
static void negatecondition (FuncState *fs, expdesc *e) {
  // 找到条件比较的指令
  Instruction *pc = getjumpcontrol(fs, e->u.info);
  lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
                                           GET_OPCODE(*pc) != OP_TEST);
  // 将原来的值取反
  SETARG_A(*pc, !(GETARG_A(*pc)));
}


/*
** Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
** is true, code will jump if 'e' is true.) Return jump position.
** Optimize when 'e' is 'not' something, inverting the condition
** and removing the 'not'.
*/
// 如果e为条件，则生成跳转指令（即，如果cond为true，如果“e”为真，代码将跳转。）返回跳转位置。
// 当“e”为某个值取反，反转条件并删除取反的条件。
static int jumponcond (FuncState *fs, expdesc *e, int cond) {
  if (e->k == VRELOCABLE) {
    Instruction ie = getinstruction(fs, e);
    // 如果e为取反
    if (GET_OPCODE(ie) == OP_NOT) {
      // 优化掉（删除）取反的指令
      fs->pc--;  /* remove previous OP_NOT */
      // 生成取反的条件跳转指令
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, !cond);
    }
    /* else go through */
  }
  // 保证表达式的值都在寄存器上 
  discharge2anyreg(fs, e);
  // 释放没用的寄存器
  freeexp(fs, e);
  // 生成条件跳转字节码
  return condjump(fs, OP_TESTSET, NO_REG, e->u.info, cond);
}


/*
** Emit code to go through if 'e' is true, jump otherwise.
*/
// 如果e为true就继续，否则跳转 
void luaK_goiftrue (FuncState *fs, expdesc *e) {
  int pc;  /* pc of new jump */
  // 调用函数将传入的表达式解析出来。
  luaK_dischargevars(fs, e);
  // 当表达式是常量（ VK ）、VKNUM （数字）以及VTRUE （布尔类型的true)时，
  // 并不需要增加一个跳转指令跳过下一条指令。
  switch (e->k) {
	// 如果是VJMP ，则说明表达式V是一个逻辑类指令，这时需要将它的跳转
	// 条件进行颠倒操作。比如，如果前面的表达式是比较变量A是否等于变量B ，那么这里
	// 会被改写成变量A是否不等于变量B 。
    case VJMP: {  /* condition? */
      negatecondition(fs, e);  /* jump when it is false */
      pc = e->u.info;  /* save jump position */
      break;
    }
    // 如果为ture，就不跳转
    case VK: case VKFLT: case VKINT: case VTRUE: {
      pc = NO_JUMP;  /* always true; do nothing */
      break;
    }

    // 最后一种是默认情况，此时需要进入jumponcond 函数中，生成针对表达
    // 式V为false情况的OP_TESTSET指令。注意，这里传入jumponcond 函数中的cond参数是0,
	// 也就是生成的是表达式为false情况下的指令
    default: {
      pc = jumponcond(fs, e, 0);  /* jump when false */
      break;
    }
  }
  // 前面根据表达式的不同类型生成跳转指令，该指令的地址返回在局部变量pc中。
  // 可以看到， pc可能有两种情况，一种为NO_JUMP ，这种情况是表达式恒为true 的’情况，
  // 其他情况最终都会生成跳转指令，而这些跳转都发生在表达式V为false 的情况。因此，
  // 这里将返回的pc变量加入到表达式的false list 中。
  luaK_concat(fs, &e->f, pc);  /* insert new jump in false list */
  // 调用luaK_patchtohere 函数，将表达式的truelist加入到jpc跳转链表中。前
  // 面已经分析过了，这在生成下一条指令时将下一条指令的pc遍历jpc链表进行回填操作。
  // 换言之，表达式E为true的情况将跳转到前面生成的跳转指令的下一条指令。
  luaK_patchtohere(fs, e->t);  /* true list jumps to here (to go through) */
  e->t = NO_JUMP;
}


/*
** Emit code to go through if 'e' is false, jump otherwise.
*/
// 如果“e”为假，则执行执行该代码，否则跳转。
void luaK_goiffalse (FuncState *fs, expdesc *e) {
  int pc;  /* pc of new jump */
  // 调用函数将传入的表达式解析出来。
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VJMP: {
      // 如果为true，直接跳转
      pc = e->u.info;  /* already jump if true */
      break;
    }
    // nil和false表示失败，不跳转，继续执行
    case VNIL: case VFALSE: {
      pc = NO_JUMP;  /* always false; do nothing */
      break;
    }
    default: {
      // 如果e的表达式的结果为true，生成条件跳转的指令
      pc = jumponcond(fs, e, 1);  /* jump if true */
      break;
    }
  }
  // 将新的跳转加入的true跳转表中
  luaK_concat(fs, &e->t, pc);  /* insert new jump in 't' list */
  // false跳转表就是直接当前指令
  luaK_patchtohere(fs, e->f);  /* false list jumps to here (to go through) */
  e->f = NO_JUMP;
}


/*
** Code 'not e', doing constant folding.
*/
// 代码：not e（e为表达式），常量折叠
static void codenot (FuncState *fs, expdesc *e) {
  // 对表达式进行预估指令生成
  luaK_dischargevars(fs, e);
  switch (e->k) {
      // nil和false取反后为true
    case VNIL: case VFALSE: {
      e->k = VTRUE;  /* true == not nil == not false */
      break;
    }
    // 常量表达式，浮点数，整数和true，取反后都是false
    case VK: case VKFLT: case VKINT: case VTRUE: {
      e->k = VFALSE;  /* false == not "x" == not 0.5 == not 1 == not true */
      break;
    }
    // 处理跳转 
    case VJMP: {
      // 对跳转指令中的条件取反
      negatecondition(fs, e);
      break;
    }
    case VRELOCABLE:
    case VNONRELOC: {
      // 保证表达式的值都在寄存器上 
      discharge2anyreg(fs, e);
      // 释放不用的寄存器
      freeexp(fs, e);
      // 生成指令
      e->u.info = luaK_codeABC(fs, OP_NOT, 0, e->u.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    default: lua_assert(0);  /* cannot happen */
  }
  /* interchange true and false lists */
  { int temp = e->f; e->f = e->t; e->t = temp; }
 
  removevalues(fs, e->f);  /* values are useless when negated */
  removevalues(fs, e->t);
}


/*
** Create expression 't[k]'. 't' must have its final result already in a
** register or upvalue.
*/
// 创建一个表达式t[k]，t必须已经取到了最终的值并且要么在寄存器，要么在upvalue上。
void luaK_indexed (FuncState *fs, expdesc *t, expdesc *k) {
  lua_assert(!hasjumps(t) && (vkisinreg(t->k) || t->k == VUPVAL));
  // table的寄存器索引或者upvalue索引
  t->u.ind.t = t->u.info;  /* register or upvalue index */
  // 键的寄存器索引或者常量表达式列表中的索引
  t->u.ind.idx = luaK_exp2RK(fs, k);  /* R/K index for key */
  t->u.ind.vt = (t->k == VUPVAL) ? VUPVAL : VLOCAL;
  // 标记为索引变量
  t->k = VINDEXED;
}


/*
** Return false if folding can raise an error.
** Bitwise operations need operands convertible to integers; division
** operations cannot have 0 as divisor.
*/
// 如果展开会引发错误，则返回 false。
// 按位运算需要可转换为整数的操作数；
// 除法运算不能有 0 作为除数。

// 判断操作符是否有效
static int validop (int op, TValue *v1, TValue *v2) {
  switch (op) {
      // 下面的操作符都是位运算相关，需要操作数都是整数
    case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
    case LUA_OPSHL: case LUA_OPSHR: case LUA_OPBNOT: {  /* conversion errors */
      lua_Integer i;
      return (tointeger(v1, &i) && tointeger(v2, &i));
    }
    // 除法不能有0作为除数
    case LUA_OPDIV: case LUA_OPIDIV: case LUA_OPMOD:  /* division by 0 */
      return (nvalue(v2) != 0);
      // 其他情况都是合法的
    default: return 1;  /* everything else is valid */
  }
}


/*
** Try to "constant-fold" an operation; return 1 iff successful.
** (In this case, 'e1' has the final result.)
*/
// 尝试常量折叠操作，如果成功就返回1
// (在这种情况下，'e1'有最后的结果)
static int constfolding (FuncState *fs, int op, expdesc *e1,
                                                const expdesc *e2) {
  TValue v1, v2, res;
  // 非数字操作符折叠可能不安全
  // 两个变量是否能转化成数字，操作符是否可用
  if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
    return 0;  /* non-numeric operands or not safe to fold */
  // 进行数学运算 
  luaO_arith(fs->ls->L, op, &v1, &v2, &res);  /* does operation */
  // 结果为整数
  if (ttisinteger(&res)) {
    e1->k = VKINT;
    e1->u.ival = ivalue(&res);
  }
  // 结果为浮点数
  else {  /* folds neither NaN nor 0.0 (to avoid problems with -0.0) */
    // 如果出现Nan和0.0，不进行常量折叠（防止出现-0.0的情况）
    lua_Number n = fltvalue(&res);
    if (luai_numisnan(n) || n == 0)
      return 0;
    e1->k = VKFLT;
    e1->u.nval = n;
  }
  return 1;
}


/*
** Emit code for unary expressions that "produce values"
** (everything but 'not').
** Expression to produce final result will be encoded in 'e'.
*/ 
// 为产生值一元表达式产生代码（不考虑not）。 表达式产生的结果最终赋值给“e”。
static void codeunexpval (FuncState *fs, OpCode op, expdesc *e, int line) {
  // 将表达式的值加载到寄存器中
  int r = luaK_exp2anyreg(fs, e);  /* opcodes operate only on registers */
  // 释放用过的寄存器 
  freeexp(fs, e);
  // 生成字节码
  e->u.info = luaK_codeABC(fs, op, 0, r, 0);  /* generate opcode */
  // 所有的操作是可以重定位
  e->k = VRELOCABLE;  /* all those operations are relocatable */
  // 修正当前位置关联的行号信息 
  luaK_fixline(fs, line);
}


/*
** Emit code for binary expressions that "produce values"
** (everything but logical operators 'and'/'or' and comparison
** operators).
** Expression to produce final result will be encoded in 'e1'.
** Because 'luaK_exp2RK' can free registers, its calls must be
** in "stack order" (that is, first on 'e2', which may have more
** recent registers to be released).
*/
// 为二元操作符产生值的代码（除逻辑运算符 and、or 和比较运算符之外的所有内容）。 
// 产生最终结果的表达式将被编码为'e1'。 因为'luaK_exp2RK'可以释放寄存器，
// 所以它的调用必须是“堆栈顺序”（也就是说，首先在'e2'上，它可能有更近的寄存器要释放）。
static void codebinexpval (FuncState *fs, OpCode op,
                           expdesc *e1, expdesc *e2, int line) {
  // 确保最终表达式结果在有效的R/K索引中
  int rk2 = luaK_exp2RK(fs, e2);  /* both operands are "RK" */
  int rk1 = luaK_exp2RK(fs, e1);
  // 释放寄存器
  freeexps(fs, e1, e2);
  // 根据操作符合操作数，生成代码
  e1->u.info = luaK_codeABC(fs, op, 0, rk1, rk2);  /* generate opcode */
  // 表达式的结果在寄存器上
  e1->k = VRELOCABLE;  /* all those operations are relocatable */
  // 修正当前位置关联的行号信息
  luaK_fixline(fs, line);
}


/*
** Emit code for comparisons.
** 'e1' was already put in R/K form by 'luaK_infix'.
*/
// 生成比较代码,e1已经通过luak_infi放入寄存器或者常量列表
static void codecomp (FuncState *fs, BinOpr opr, expdesc *e1, expdesc *e2) {
  // 得到比较的左右两个值
  int rk1 = (e1->k == VK) ? RKASK(e1->u.info)
                          : check_exp(e1->k == VNONRELOC, e1->u.info);
  int rk2 = luaK_exp2RK(fs, e2);
  // 释放寄存器
  freeexps(fs, e1, e2);
  // 根据不同的比较操作符处理
  switch (opr) {
    case OPR_NE: {  /* '(a ~= b)' ==> 'not (a == b)' */
      // 将不等于换成等于取反，生成“条件跳转”代码,
      e1->u.info = condjump(fs, OP_EQ, 0, rk1, rk2);
      break;
    }
    case OPR_GT: case OPR_GE: {
      /* '(a > b)' ==> '(b < a)';  '(a >= b)' ==> '(b <= a)' */
      // 将大于和大于等于分别换成小于、小于等于，生成条件跳转代码
      OpCode op = cast(OpCode, (opr - OPR_NE) + OP_EQ);
      e1->u.info = condjump(fs, op, 1, rk2, rk1);  /* invert operands */
      break;
    }
    default: {  /* '==', '<', '<=' use their own opcodes */
      // 生成条件跳转代码
      OpCode op = cast(OpCode, (opr - OPR_EQ) + OP_EQ);
      e1->u.info = condjump(fs, op, 1, rk1, rk2);
      break;
    }
  }
  e1->k = VJMP;
}


/*
** Aplly prefix operation 'op' to expression 'e'.
*/
// 应用前缀操作符op到表达式e上
void luaK_prefix (FuncState *fs, UnOpr op, expdesc *e, int line) {
  // 虚构第二个假的操作数
  static const expdesc ef = {VKINT, {0}, NO_JUMP, NO_JUMP};
  switch (op) {
    case OPR_MINUS: case OPR_BNOT:  /* use 'ef' as fake 2nd operand */
      // 使用'ef'作为假的第二个操作数
      // 进行常量折叠
      if (constfolding(fs, op + LUA_OPUNM, e, &ef))
        break;
      /* FALLTHROUGH */
    // 取长度
    case OPR_LEN:
      // 为产生值一元表达式产生代码（不考虑not)
      codeunexpval(fs, cast(OpCode, op + OP_UNM), e, line);
      break;
    // not表达式编码
    case OPR_NOT: codenot(fs, e); break;
    default: lua_assert(0);
  }
}


/*
** Process 1st operand 'v' of binary operation 'op' before reading
** 2nd operand.
*/
// 在读取第二个操作数之前处理二元运算“op”的第一个操作数“v”。
void luaK_infix (FuncState *fs, BinOpr op, expdesc *v) {
  switch (op) {
    // and
    case OPR_AND: {
      // 如果为true，继续，false，就跳转
      luaK_goiftrue(fs, v);  /* go ahead only if 'v' is true */
      break;
    }
    // or
    case OPR_OR: {
      // 如果为false，继续，ture，跳转
      luaK_goiffalse(fs, v);  /* go ahead only if 'v' is false */
      break;
    }
    // 连接符(...)
    case OPR_CONCAT: {
      // 解析表达式,操作数必须在“堆栈”上
      luaK_exp2nextreg(fs, v);  /* operand must be on the 'stack' */
      break;
    }
    // 二元数值运算符
    case OPR_ADD: case OPR_SUB:
    case OPR_MUL: case OPR_DIV: case OPR_IDIV:
    case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
      // 将表达式转换为数值
      if (!tonumeral(v, NULL))
        // 确保最终表达式结果在有效的R(寄存器)/K（常量）索引中
        luaK_exp2RK(fs, v);
      /* else keep numeral, which may be folded with 2nd operand */
      break;
    }
    default: {
      // 确保最终表达式结果在有效的R(寄存器)/K（常量）索引中
      luaK_exp2RK(fs, v);
      break;
    }
  }
}


/*
** Finalize code for binary operation, after reading 2nd operand.
** For '(a .. b .. c)' (which is '(a .. (b .. c))', because
** concatenation is right associative), merge second CONCAT into first
** one.
*/
// 读取第二个操作数后，完成二元操作符的代码。 对于“(a .. b .. c)”
// （即“(a .. (b .. c))”，因为串联是右结合），将第二个 CONCAT 合并到第一个。
void luaK_posfix (FuncState *fs, BinOpr op,
                  expdesc *e1, expdesc *e2, int line) {
  switch (op) {
    case OPR_AND: {
      lua_assert(e1->t == NO_JUMP);  /* list closed by 'luK_infix' */
      // 解析表达式
      luaK_dischargevars(fs, e2);
      // 连接判断失败跳转列表
      luaK_concat(fs, &e2->f, e1->f);
      *e1 = *e2;
      break;
    }
    case OPR_OR: {
      lua_assert(e1->f == NO_JUMP);  /* list closed by 'luK_infix' */
      // 解析表达式
      luaK_dischargevars(fs, e2);
      // 连接判断成功跳转列表
      luaK_concat(fs, &e2->t, e1->t);
      *e1 = *e2;
      break;
    }
    case OPR_CONCAT: {
      //  确保最终表达式结果在寄存器中或者是常量
      luaK_exp2val(fs, e2);
      // 寄存器上
      if (e2->k == VRELOCABLE &&
          GET_OPCODE(getinstruction(fs, e2)) == OP_CONCAT) {
        lua_assert(e1->u.info == GETARG_B(getinstruction(fs, e2))-1);
        // 释放e1的寄存器
        freeexp(fs, e1);
        // e2指令的B的值为e1的指令索引
        SETARG_B(getinstruction(fs, e2), e1->u.info);
        e1->k = VRELOCABLE; 
        // e2的指令索引赋值给e1的指令索引
        e1->u.info = e2->u.info;
      }
      else {
        // 确保表达式的结果（包括从它的跳转表的结果）保存在下一个可用的寄存器中
        luaK_exp2nextreg(fs, e2);  /* operand must be on the 'stack' */
       //  为二元操作符产生值的代码（
        codebinexpval(fs, OP_CONCAT, e1, e2, line);
      }
      break;
    }
    case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
    case OPR_IDIV: case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
      // 先尝试进行常量折叠操作，不行的话，再产生二元操作符处理代码
      if (!constfolding(fs, op + LUA_OPADD, e1, e2))
        codebinexpval(fs, cast(OpCode, op + OP_ADD), e1, e2, line);
      break;
    }
    // 比较操作
    case OPR_EQ: case OPR_LT: case OPR_LE:
    case OPR_NE: case OPR_GT: case OPR_GE: {
      codecomp(fs, op, e1, e2);
      break;
    }
    default: lua_assert(0);
  }
}


/*
** Change line information associated with current position.
*/
// 修正当前位置关联的行号信息
void luaK_fixline (FuncState *fs, int line) {
  fs->f->lineinfo[fs->pc - 1] = line;
}


/*
** Emit a SETLIST instruction.
** 'base' is register that keeps table;
** 'nelems' is #table plus those to be stored now;
** 'tostore' is number of values (in registers 'base + 1',...) to add to
** table (or LUA_MULTRET to add up to stack top).
*/
// 发出 SETLIST 指令。
// 'base' 是保存表的寄存器； 
// 'nelems' 是#table 加上那些现在要存储的；'
//  tostore' 是要添加到表（或 LUA_MULTRET 以添加到堆栈顶部）的值的数量（在寄存器'base + 1'中，...）。
void luaK_setlist (FuncState *fs, int base, int nelems, int tostore) {
  // 计算第几次的setlist
  int c =  (nelems - 1)/LFIELDS_PER_FLUSH + 1;
  // 这次setlist的数目
  int b = (tostore == LUA_MULTRET) ? 0 : tostore;
  lua_assert(tostore != 0 && tostore <= LFIELDS_PER_FLUSH);
  // 数量比较少，直接编码SETLIST
  if (c <= MAXARG_C)
    luaK_codeABC(fs, OP_SETLIST, base, b, c);
  // 数量很多了，需要增加额外的指令辅助
  else if (c <= MAXARG_Ax) {
    // 构造setlist指令
    luaK_codeABC(fs, OP_SETLIST, base, b, 0);
    // 构造“额外参数”指令（格式为“iAx”）
    codeextraarg(fs, c);
  }
  // 数量太多，包语法错误
  else
    luaX_syntaxerror(fs->ls, "constructor too long");
  // 释放base以后的寄存器
  fs->freereg = base + 1;  /* free registers with list values */
}

