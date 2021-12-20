/*
** $Id: lgc.h,v 2.91.1.1 2017/04/19 17:39:34 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"
#include "lstate.h"

/*
** Collectable objects may have one of three colors: white, which
** means the object is not marked; gray, which means the
** object is marked, but its references may be not marked; and
** black, which means that the object and all its references are marked.
** The main invariant of the garbage collector, while marking objects,
** is that a black object can never point to a white one. Moreover,
** any gray object must be in a "gray list" (gray, grayagain, weak,
** allweak, ephemeron) so that it can be visited again before finishing
** the collection cycle. These lists have no meaning when the invariant
** is not being enforced (e.g., sweep phase).
*/

/* how much to allocate before next GC step */
// 在下一个 GC 步骤之前分配多少
#if !defined(GCSTEPSIZE)
/* ~100 small strings */
// 100个短字符串
#define GCSTEPSIZE	(cast_int(100 * sizeof(TString)))
#endif

// Lua可回收对象有三种颜色, 白色、灰色和黑色.
// 白色表示没有被标记的对象, 白色有两种每次完整GC后切换;
// 灰色表示标记过的对象, 但可能引用未标记的对象;
// 黑色表示标记过的对象, 并且它引用的对象都标记过;

/*
** Possible states of the Garbage Collector
*/
// 传播阶段：标记对象
#define GCSpropagate	0
// 原子阶段：一次性标记
#define GCSatomic	1
// 清扫allgc
#define GCSswpallgc	2
// 清扫finobj
#define GCSswpfinobj	3
// 清扫tobefnz
#define GCSswptobefnz	4
// 清扫结束
#define GCSswpend	5
// 调用终结函数(__gc)
#define GCScallfin	6
// 暂停阶段
#define GCSpause	7

// 是否在清扫阶段
#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep
** phase may break the invariant, as objects turned white may point to
** still-black objects. The invariant is restored when sweep ends and
** all objects are white again.
*/

#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


/*
** some useful bit tricks
*/
// 
#define resetbits(x,m)		((x) &= cast(lu_byte, ~(m)))
#define setbits(x,m)		((x) |= (m))
#define testbits(x,m)		((x) & (m))
#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


/* Layout for bit use in 'marked' field: */
#define WHITE0BIT	0  // 对象为白色类型0 /* object is white (type 0) */
#define WHITE1BIT	1  // 对象为白色类型1 /* object is white (type 1) */
#define BLACKBIT	2  // 对象为黑色 /* object is black */
// FINALIZEDBIT 用于标记没有被引用需要回收的udata 。udata的处理与其他数据类型不同，由于它是用户传人的数据，它的回收可能会调用用户注册的GC 函数，
#define FINALIZEDBIT	3	// 对象被标记为可以回收的 //  /* object has been marked for finalization */
/* bit 7 is currently used by tests (luaL_checkmemory) */

// 所有的白色的位与在一起
#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)

// 是否是白色
#define iswhite(x)      testbits((x)->marked, WHITEBITS)
// 是否是黑色
#define isblack(x)      testbit((x)->marked, BLACKBIT)
// 是否是灰色（不是白色也不是黑色）
#define isgray(x)  /* neither white nor black */  \
	(!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))
// 是否是可以回收的了
#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)
// otherwhite用来判断当前的白色用于回收
#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
#define isdeadm(ow,m)	(!(((m) ^ WHITEBITS) & (ow)))
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)
// 将当前的白色转换成另外一种
#define changewhite(x)	((x)->marked ^= WHITEBITS)
// 设置成黑色
#define gray2black(x)	l_setbit((x)->marked, BLACKBIT)
// 当前的白色
#define luaC_white(g)	cast(lu_byte, (g)->currentwhite & WHITEBITS)


/*
** Does one step of collection when debt becomes positive. 'pre'/'pos'
** allows some adjustments to be done only when needed. macro
** 'condchangemem' is used only for heavy tests (forcing a full
** GC cycle on every opportunity)
*/
// 当GCdebt变成正数时，进行一次收集，'pre'/'pos'宏允许一些需要的调整。
// 宏 'condchangemem'只用于重的测试（每一次机会强制一个完整的GC循环）
#define luaC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

/* more often than not, 'pre'/'pos' are empty */
// 通常情况下，'pre'/'pos'是空的
#define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)

#define luaC_barrier(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ?  \
	luaC_barrier_(L,obj2gco(p),gcvalue(v)) : cast_void(0))

#define luaC_barrierback(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ? \
	luaC_barrierback_(L,p) : cast_void(0))

#define luaC_objbarrier(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? \
	luaC_barrier_(L,obj2gco(p),obj2gco(o)) : cast_void(0))

#define luaC_upvalbarrier(L,uv) ( \
	(iscollectable((uv)->v) && !upisopen(uv)) ? \
         luaC_upvalbarrier_(L,uv) : cast_void(0))

LUAI_FUNC void luaC_fix (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_freeallobjects (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC void luaC_runtilstate (lua_State *L, int statesmask);
LUAI_FUNC void luaC_fullgc (lua_State *L, int isemergency);
LUAI_FUNC GCObject *luaC_newobj (lua_State *L, int tt, size_t sz);
LUAI_FUNC void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v);
LUAI_FUNC void luaC_barrierback_ (lua_State *L, Table *o);
LUAI_FUNC void luaC_upvalbarrier_ (lua_State *L, UpVal *uv);
LUAI_FUNC void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt);
LUAI_FUNC void luaC_upvdeccount (lua_State *L, UpVal *uv);


#endif
