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
	���Ǽ���ָ�����޷�����,����ָ��ĸ�6λΪ�����룬ָ�����������ֶΣ�
	'A' : 8λ
	'B' : 9λ
	'C' : 9λ
	'Ax'��26λ('A', 'B'�� 'C'������һ��,8+9+9=26)
	'Bx'��18λ('B' and 'C'������һ��)
	'sBx'�������ŵ�Bx
*/

// ������ָ���ʽ
// (iABC)	������  A	B	C
// (iABx)	������	A	Bx
// (iAsBx)	������	A	sBx
// (iAx)	������	Ax
enum OpMode {iABC, iABx, iAsBx, iAx};  /* basic instruction format */


/*
** size and position of opcode arguments.
*/
// ���������Ĵ�С��λ�� ��С���Ƕ��ǣ�λ
#define SIZE_C		9
#define SIZE_B		9
#define SIZE_Bx		(SIZE_C + SIZE_B)					// 18
#define SIZE_A		8
#define SIZE_Ax		(SIZE_C + SIZE_B + SIZE_A)			// 26

// ������λ��С
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
// ��������������ơ�
// ����ʹ�ã��з��ŵģ�int �����������������
// �������Ǳ����ʺ� LUAI_BITSINT - 1 λ�� - 1 ��ʾ���ţ�
// Bx�����ֵ
#if SIZE_Bx < LUAI_BITSINT-1
// �޷��������ֵ
#define MAXARG_Bx        ((1<<SIZE_Bx)-1)
// �з��������ֵ
#define MAXARG_sBx        (MAXARG_Bx>>1)        // sBx��ʾ�з��ŵ�Bx  /* 'sBx' is signed */
#else
#define MAXARG_Bx        MAX_INT
#define MAXARG_sBx        MAX_INT
#endif

// Ax�����ֵ
#if SIZE_Ax < LUAI_BITSINT-1
#define MAXARG_Ax	((1<<SIZE_Ax)-1)
#else
#define MAXARG_Ax	MAX_INT
#endif

// A B C�����������ֵ
#define MAXARG_A        ((1<<SIZE_A)-1)
#define MAXARG_B        ((1<<SIZE_B)-1)
#define MAXARG_C        ((1<<SIZE_C)-1)


/* creates a mask with 'n' 1 bits at position 'p' */
// ��һ������0ȡ���õ�0xFFFFFFFF
// �ڶ���������nλ��������߲��ֶ���1���ұ߲��ֶ���0��
// ��������ȡ����������߲��ֶ���0���ұ߲��ֶ���1
// ���Ĳ���������pλ
// ���Ǵӵ�pλ��ʼ�����nλ����1
// MASK1(8,3)��ֵ�ǣ�011111111000
// MASK1(7,4)��ֵ�ǣ�011111110000
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* creates a mask with 'n' 0 bits at position 'p' */
// ����һ������ӵ�pλ��ʼ�����nλ����0
// MASK0(7, 4)��ֵ�ǣ�1111111111111111111111111111111111111111111111111111100000001111
// MASK0(8, 3)��ֵ�ǣ�1111111111111111111111111111111111111111111111111111100000000111
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/
// ȡ�������ֵ,���ò������ֵ
// ȡ��SIZE_OP(6)λ
#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(Instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))

#define getarg(i,pos,size)	(cast(int, ((i)>>pos) & MASK1(size,0)))
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                ((cast(Instruction, v)<<pos)&MASK1(size,pos))))

// ȡ����A��ֵ,���ò���A��ֵ
#define GETARG_A(i)	getarg(i, POS_A, SIZE_A)
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)
// ȡ����B��ֵ�����ò���B��ֵ
#define GETARG_B(i)	getarg(i, POS_B, SIZE_B)
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)
// ȡ����C��ֵ�����ò���C��ֵ
#define GETARG_C(i)	getarg(i, POS_C, SIZE_C)
#define SETARG_C(i,v)	setarg(i, v, POS_C, SIZE_C)

// ȡ����Bx��ֵ�����ò���Bx��ֵ
#define GETARG_Bx(i)	getarg(i, POS_Bx, SIZE_Bx)
#define SETARG_Bx(i,v)	setarg(i, v, POS_Bx, SIZE_Bx)

// ȡ����Ax��ֵ�����ò���Ax��ֵ
#define GETARG_Ax(i)	getarg(i, POS_Ax, SIZE_Ax)
#define SETARG_Ax(i,v)	setarg(i, v, POS_Ax, SIZE_Ax)
// ȡ����sBx��ֵ�����ò���sBx��ֵ
#define GETARG_sBx(i)	(GETARG_Bx(i)-MAXARG_sBx)
#define SETARG_sBx(i,b)	SETARG_Bx((i),cast(unsigned int, (b)+MAXARG_sBx))

// ����ָ�iABC��ʽ
#define CREATE_ABC(o,a,b,c)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, b)<<POS_B) \
			| (cast(Instruction, c)<<POS_C))

// ����ָ�iABx��ʽ
#define CREATE_ABx(o,a,bc)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, bc)<<POS_Bx))

// ����ָ�iAx��ʽ
#define CREATE_Ax(o,a)		((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_Ax))


/*
** Macros to operate RK indices
*/
// ����������������ĺ���ͺܼ��ˣ��ж�������ݵĵڰ�λ�ǲ���l ������ǣ�����Ϊ
// Ӧ�ô�K�����л�ȡ���ݣ�������ǴӺ���ս�Ĵ����л�ȡ���ݡ�������Ͼ����ָ��������
// �����ʽ��
/* this bit 1 means constant (0 means register) */
// ��λΪ1��ʾ������0��ʾ�Ĵ���
#define BITRK		(1 << (SIZE_B - 1))

/* test whether value is a constant */
// �Ƿ��ǳ���
#define ISK(x)		((x) & BITRK)

/* gets the index of the constant */
// �õ�����������
#define INDEXK(r)	((int)(r) & ~BITRK)

#if !defined(MAXINDEXRK)  /* (for debugging only) */
#define MAXINDEXRK	(BITRK - 1)
#endif

/* code a constant index as a RK value */
// ��������������ΪRKֵ
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
// ָ��ö�٣�����޸���ö�ٵģ�ע���޸ġ�ORDER OP���ĵط�
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
// ָ���������롣��ʽ�ǣ�
// 0-1λ��������
// 2-3λ��C������
// 4-5λ��B������
// 6λ����ʾ���ָ��᲻�ḳֵ���Ĵ���A��R��A����
// 7λ����ʾ���ǲ���һ���߼�������ص�ָ�����һ��ָ���������ת��

// B��C������ʽ
enum OpArgMask {
  OpArgN,  // ����δʹ�� /* argument is not used */
  OpArgU,  // ��ʹ�ò��� /* argument is used */
  OpArgR,  // �ò����ǼĴ�������תƫ�� /* argument is a register or a jump offset */
  OpArgK   // �ò����ǳ������ǼĴ��� /* argument is a constant or register/constant */
};

LUAI_DDEC const lu_byte luaP_opmodes[NUM_OPCODES];

// ��Ӧ��opmode�ĺ�
// t����ʾ���ǲ���һ���߼�������ص�ָ��
// a����ʾ���ָ��᲻�ḳֵ��R��A��
// b/c��B��C�Ĳ�����ʽ
// mode�����OpCode�ĸ�ʽ
// ��7λ��t
// ��6λ��a
// ��5λ����4λ��2λ��4-5����b
// ��3λ����2λ��2λ��2-3����c
// ��1λ����0λ��2λ��0-1����m
// ȡ2λ����0-1λ���õ�������ģʽ
#define getOpMode(m)	(cast(enum OpMode, luaP_opmodes[m] & 3))
// ȡ2λ����2-3λ���õ�B��ģʽ
#define getBMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 4) & 3))
// ȡ2λ����4-5λ���õ�C��ģʽ
#define getCMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 2) & 3))
// ȡ1λ����6λ�����A��ģʽ
#define testAMode(m)	(luaP_opmodes[m] & (1 << 6))
// ȡ1λ����7λ�����T��ģʽ
#define testTMode(m)	(luaP_opmodes[m] & (1 << 7))


LUAI_DDEC const char *const luaP_opnames[NUM_OPCODES+1];  /* opcode names */


/* number of list items to accumulate before a SETLIST instruction */
#define LFIELDS_PER_FLUSH	50


#endif
