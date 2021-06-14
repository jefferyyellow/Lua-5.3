/*
** $Id: lopcodes.h,v 1.149.1.1 2017/04/19 17:20:42 roberto Exp $
** Opcodes for Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lopcodes_h
#define lopcodes_h

#include "llimits.h"


/*===========================================================================
  We assume that instructions are unsigned numbers.
  All instructions have an opcode in the first 6 bits.
  Instructions can have the following fields:
	'A' : 8 bits
	'B' : 9 bits
	'C' : 9 bits
	'Ax' : 26 bits ('A', 'B', and 'C' together)
	'Bx' : 18 bits ('B' and 'C' together)
	'sBx' : signed Bx

  A signed argument is represented in excess K; that is, the number
  value is the unsigned value minus K. K is exactly the maximum value
  for that argument (so that -max is represented by 0, and +max is
  represented by 2*max), which is half the maximum for the corresponding
  unsigned argument.
===========================================================================*/
/*
	我们假设指令是无符号数,所有指令的高6位为操作码，指令包含下面的字段：
	'A' : 8位
	'B' : 9位
	'C' : 9位
	'Ax'：26位('A', 'B'和 'C'整合在一起,8+9+9=26)
	'Bx'：18位('B' and 'C'整合在一起)
	'sBx'：带符号的Bx
*/

// 基本的指令格式
// (iABC)	操作码  A	B	C
// (iABx)	操作码	A	Bx
// (iAsBx)	操作码	A	sBx
// (iAx)	操作码	Ax
enum OpMode {iABC, iABx, iAsBx, iAx};  /* basic instruction format */


/*
** size and position of opcode arguments.
*/
// 操作参数的大小和位置 大小但是都是：位
#define SIZE_C		9
#define SIZE_B		9
#define SIZE_Bx		(SIZE_C + SIZE_B)					// 18
#define SIZE_A		8
#define SIZE_Ax		(SIZE_C + SIZE_B + SIZE_A)			// 26

// 操作码位大小
#define SIZE_OP		6

#define POS_OP		0
#define POS_A		(POS_OP + SIZE_OP)		// 6
#define POS_C		(POS_A + SIZE_A)		// 14
#define POS_B		(POS_C + SIZE_C)		// 23
#define POS_Bx		POS_C					// 23
#define POS_Ax		POS_A					// 6


/*
** limits for opcode arguments.
** we use (signed) int to manipulate most arguments,
** so they must fit in LUAI_BITSINT-1 bits (-1 for sign)
*/
// 操作码参数的限制。
// 我们使用（有符号的）int 来操作大多数参数，
// 所以它们必须适合 LUAI_BITSINT - 1 位（ - 1 表示符号）
// Bx的最大值
#if SIZE_Bx < LUAI_BITSINT-1
// 无符号数最大值
#define MAXARG_Bx        ((1<<SIZE_Bx)-1)
// 有符号数最大值
#define MAXARG_sBx        (MAXARG_Bx>>1)        // sBx表示有符号的Bx  /* 'sBx' is signed */
#else
#define MAXARG_Bx        MAX_INT
#define MAXARG_sBx        MAX_INT
#endif

// Ax的最大值
#if SIZE_Ax < LUAI_BITSINT-1
#define MAXARG_Ax	((1<<SIZE_Ax)-1)
#else
#define MAXARG_Ax	MAX_INT
#endif

// A B C操作数的最大值
#define MAXARG_A        ((1<<SIZE_A)-1)
#define MAXARG_B        ((1<<SIZE_B)-1)
#define MAXARG_C        ((1<<SIZE_C)-1)


/* creates a mask with 'n' 1 bits at position 'p' */
// 第一步：将0取反得到0xFFFFFFFF
// 第二步：左移n位，导致左边部分都是1，右边部分都是0，
// 第三步：取反，导致左边部分都是0，右边部分都是1
// 第四步：再左移p位
// 就是从第p位开始的左边n位都是1
// MASK1(8,3)的值是：011111111000
// MASK1(7,4)的值是：011111110000
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* creates a mask with 'n' 0 bits at position 'p' */
// 创建一个掩码从第p位开始的左边n位都是0
// MASK0(7, 4)的值是：1111111111111111111111111111111111111111111111111111100000001111
// MASK0(8, 3)的值是：1111111111111111111111111111111111111111111111111111100000000111
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/
// 取操作码的值,设置操作码的值
// 取低SIZE_OP(6)位
#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(Instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))

#define getarg(i,pos,size)	(cast(int, ((i)>>pos) & MASK1(size,0)))
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                ((cast(Instruction, v)<<pos)&MASK1(size,pos))))

// 取参数A的值,设置参数A的值
#define GETARG_A(i)	getarg(i, POS_A, SIZE_A)
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)
// 取参数B的值，设置参数B的值
#define GETARG_B(i)	getarg(i, POS_B, SIZE_B)
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)
// 取参数C的值，设置参数C的值
#define GETARG_C(i)	getarg(i, POS_C, SIZE_C)
#define SETARG_C(i,v)	setarg(i, v, POS_C, SIZE_C)

// 取参数Bx的值，设置参数Bx的值
#define GETARG_Bx(i)	getarg(i, POS_Bx, SIZE_Bx)
#define SETARG_Bx(i,v)	setarg(i, v, POS_Bx, SIZE_Bx)

// 取参数Ax的值，设置参数Ax的值
#define GETARG_Ax(i)	getarg(i, POS_Ax, SIZE_Ax)
#define SETARG_Ax(i,v)	setarg(i, v, POS_Ax, SIZE_Ax)
// 取参数sBx的值，设置参数sBx的值
#define GETARG_sBx(i)	(GETARG_Bx(i)-MAXARG_sBx)
#define SETARG_sBx(i,b)	SETARG_Bx((i),cast(unsigned int, (b)+MAXARG_sBx))

// 创建指令，iABC格式
#define CREATE_ABC(o,a,b,c)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, b)<<POS_B) \
			| (cast(Instruction, c)<<POS_C))

// 创建指令，iABx格式
#define CREATE_ABx(o,a,bc)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, bc)<<POS_Bx))

// 创建指令，iAx格式
#define CREATE_Ax(o,a)		((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_Ax))


/*
** Macros to operate RK indices
*/
// 结合起来看，这个宏的含义就很简单了：判断这个数据的第八位是不是l ，如果是，则认为
// 应该从K数组中获取数据，否则就是从函数战寄存器中获取数据。后面会结合具体的指令来解释
// 这个格式。
/* this bit 1 means constant (0 means register) */
// 该位为1表示常量，0表示寄存器
#define BITRK		(1 << (SIZE_B - 1))

/* test whether value is a constant */
// 是否是常量
#define ISK(x)		((x) & BITRK)

/* gets the index of the constant */
// 得到常量的索引
#define INDEXK(r)	((int)(r) & ~BITRK)

#if !defined(MAXINDEXRK)  /* (for debugging only) */
#define MAXINDEXRK	(BITRK - 1)
#endif

/* code a constant index as a RK value */
// 将常量索引编码为RK值
#define RKASK(x)	((x) | BITRK)


/*
** invalid register that fits in 8 bits
*/
#define NO_REG		MAXARG_A


/*
** R(x) - register
** Kst(x) - constant (in constant table)
** RK(x) == if ISK(x) then Kst(INDEXK(x)) else R(x)
*/


/*
** grep "ORDER OP" if you change these enums
*/
// 指令枚举，如果修改了枚举的，注意修改“ORDER OP”的地方
typedef enum {
/*----------------------------------------------------------------------
name		args	description
------------------------------------------------------------------------*/
OP_MOVE,		/*	A B		R(A) := R(B)					*/
OP_LOADK,		/*	A Bx	R(A) := Kst(Bx)					*/
OP_LOADKX,		/*	A 		R(A) := Kst(extra arg)				*/
OP_LOADBOOL,	/*	A B C	R(A) := (Bool)B; if (C) pc++			*/
OP_LOADNIL,		/*	A B		R(A), R(A+1), ..., R(A+B) := nil		*/
OP_GETUPVAL,	/*	A B		R(A) := UpValue[B]				*/

OP_GETTABUP,	/*	A B C	R(A) := UpValue[B][RK(C)]			*/
OP_GETTABLE,	/*	A B C	R(A) := R(B)[RK(C)]				*/

OP_SETTABUP,	/*	A B C	UpValue[A][RK(B)] := RK(C)			*/
OP_SETUPVAL,	/*	A B		UpValue[B] := R(A)				*/
OP_SETTABLE,	/*	A B C	R(A)[RK(B)] := RK(C)				*/

OP_NEWTABLE,	/*	A B C	R(A) := {} (size = B,C)				*/

OP_SELF,		/*	A B C	R(A+1) := R(B); R(A) := R(B)[RK(C)]		*/

OP_ADD,			/*	A B C	R(A) := RK(B) + RK(C)				*/
OP_SUB,			/*	A B C	R(A) := RK(B) - RK(C)				*/
OP_MUL,			/*	A B C	R(A) := RK(B) * RK(C)				*/
OP_MOD,			/*	A B C	R(A) := RK(B) % RK(C)				*/
OP_POW,			/*	A B C	R(A) := RK(B) ^ RK(C)				*/
OP_DIV,			/*	A B C	R(A) := RK(B) / RK(C)				*/
OP_IDIV,		/*	A B C	R(A) := RK(B) // RK(C)				*/
OP_BAND,		/*	A B C	R(A) := RK(B) & RK(C)				*/
OP_BOR,			/*	A B C	R(A) := RK(B) | RK(C)				*/
OP_BXOR,		/*	A B C	R(A) := RK(B) ~ RK(C)				*/
OP_SHL,			/*	A B C	R(A) := RK(B) << RK(C)				*/
OP_SHR,			/*	A B C	R(A) := RK(B) >> RK(C)				*/
OP_UNM,			/*	A B		R(A) := -R(B)					*/
OP_BNOT,		/*	A B		R(A) := ~R(B)					*/
OP_NOT,			/*	A B		R(A) := not R(B)				*/
OP_LEN,			/*	A B		R(A) := length of R(B)				*/

OP_CONCAT,		/*	A B C	R(A) := R(B).. ... ..R(C)			*/

OP_JMP,			/*	A sBx	pc+=sBx; if (A) close all upvalues >= R(A - 1)	*/
OP_EQ,			/*	A B C	if ((RK(B) == RK(C)) ~= A) then pc++		*/
OP_LT,			/*	A B C	if ((RK(B) <  RK(C)) ~= A) then pc++		*/
OP_LE,			/*	A B C	if ((RK(B) <= RK(C)) ~= A) then pc++		*/

OP_TEST,		/*	A C		if not (R(A) <=> C) then pc++			*/
OP_TESTSET,		/*	A B C	if (R(B) <=> C) then R(A) := R(B) else pc++	*/

OP_CALL,		/*	A B C	R(A), ... ,R(A+C-2) := R(A)(R(A+1), ... ,R(A+B-1)) */
OP_TAILCALL,	/*	A B C	return R(A)(R(A+1), ... ,R(A+B-1))		*/
OP_RETURN,		/*	A B		return R(A), ... ,R(A+B-2)	(see note)	*/

OP_FORLOOP,		/*	A sBx	R(A)+=R(A+2);
							if R(A) <?= R(A+1) then { pc+=sBx; R(A+3)=R(A) }*/
OP_FORPREP,		/*	A sBx	R(A)-=R(A+2); pc+=sBx				*/

OP_TFORCALL,	/*	A C		R(A+3), ... ,R(A+2+C) := R(A)(R(A+1), R(A+2));	*/
OP_TFORLOOP,	/*	A sBx	if R(A+1) ~= nil then { R(A)=R(A+1); pc += sBx }*/

OP_SETLIST,		/*	A B C	R(A)[(C-1)*FPF+i] := R(A+i), 1 <= i <= B	*/

OP_CLOSURE,		/*	A Bx	R(A) := closure(KPROTO[Bx])			*/

OP_VARARG,		/*	A B		R(A), R(A+1), ..., R(A+B-2) = vararg		*/

OP_EXTRAARG		/*	Ax		extra (larger) argument for previous opcode	*/
} OpCode;


#define NUM_OPCODES	(cast(int, OP_EXTRAARG) + 1)



/*===========================================================================
  Notes:
  (*) In OP_CALL, if (B == 0) then B = top. If (C == 0), then 'top' is
  set to last_result+1, so next open instruction (OP_CALL, OP_RETURN,
  OP_SETLIST) may use 'top'.

  (*) In OP_VARARG, if (B == 0) then use actual number of varargs and
  set top (like in OP_CALL with C == 0).

  (*) In OP_RETURN, if (B == 0) then return up to 'top'.

  (*) In OP_SETLIST, if (B == 0) then B = 'top'; if (C == 0) then next
  'instruction' is EXTRAARG(real C).

  (*) In OP_LOADKX, the next 'instruction' is always EXTRAARG.

  (*) For comparisons, A specifies what condition the test should accept
  (true or false).

  (*) All 'skips' (pc++) assume that next instruction is a jump.

===========================================================================*/


/*
** masks for instruction properties. The format is:
** bits 0-1: op mode
** bits 2-3: C arg mode
** bits 4-5: B arg mode
** bit 6: instruction set register A
** bit 7: operator is a test (next instruction must be a jump)
*/
// 指令属性掩码。格式是：
// 0-1位：操作码
// 2-3位：C参数码
// 4-5位：B参数码
// 6位：表示这个指令会不会赋值给寄存器A【R（A）】
// 7位：表示这是不是一条逻辑测试相关的指令，（下一条指令必须是跳转）

// B、C参数格式
enum OpArgMask {
  OpArgN,  // 参数未使用 /* argument is not used */
  OpArgU,  // 已使用参数 /* argument is used */
  OpArgR,  // 该参数是寄存器或跳转偏移 /* argument is a register or a jump offset */
  OpArgK   // 该参数是常量还是寄存器 /* argument is a constant or register/constant */
};

LUAI_DDEC const lu_byte luaP_opmodes[NUM_OPCODES];

// 对应与opmode的宏
// t：表示这是不是一条逻辑测试相关的指令
// a：表示这个指令会不会赋值给R（A）
// b/c：B、C的参数格式
// mode：这个OpCode的格式
// 第7位：t
// 第6位：a
// 第5位，第4位（2位：4-5）：b
// 第3位，第2位（2位：2-3）：c
// 第1位，第0位（2位：0-1）：m
// 取2位：第0-1位，得到操作码模式
#define getOpMode(m)	(cast(enum OpMode, luaP_opmodes[m] & 3))
// 取2位：第2-3位，得到B的模式
#define getBMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 4) & 3))
// 取2位：第4-5位，得到C的模式
#define getCMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 2) & 3))
// 取1位，第6位，检查A的模式
#define testAMode(m)	(luaP_opmodes[m] & (1 << 6))
// 取1位，第7位，检查T的模式
#define testTMode(m)	(luaP_opmodes[m] & (1 << 7))


LUAI_DDEC const char *const luaP_opnames[NUM_OPCODES+1];  /* opcode names */


/* number of list items to accumulate before a SETLIST instruction */
#define LFIELDS_PER_FLUSH	50


#endif
