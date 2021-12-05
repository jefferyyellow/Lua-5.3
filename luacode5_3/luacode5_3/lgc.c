/*
** $Id: lgc.c,v 2.215.1.2 2017/08/31 16:15:27 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#define lgc_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


/*
** internal state for collector while inside the atomic phase. The
** collector should never be in this state while running regular code.
*/
#define GCSinsideatomic		(GCSpause + 1)

/*
** cost of sweeping one element (the size of a small object divided
** by some adjust for the sweep speed)
*/
// 扫描一个元素的成本（一个小物体的大小除以通过一些扫描速度的调整）
#define GCSWEEPCOST	((sizeof(TString) + 4) / 4)

/* maximum number of elements to sweep in each single step */
// 每一步中要扫描的最大元素数
#define GCSWEEPMAX	(cast_int((GCSTEPSIZE / GCSWEEPCOST) / 4))

/* cost of calling one finalizer */
// 调用一个finalizer的成本
#define GCFINALIZECOST	GCSWEEPCOST


/*
** macro to adjust 'stepmul': 'stepmul' is actually used like
** 'stepmul / STEPMULADJ' (value chosen by tests)
*/
// 用于调整stepmul的宏，stepmul实际用于像stepmul/STEPMULADJ
#define STEPMULADJ		200


/*
** macro to adjust 'pause': 'pause' is actually used like
** 'pause / PAUSEADJ' (value chosen by tests)
*/
// 用于调整pause的宏：pause实际用于像pause/PAUSEADJ
#define PAUSEADJ		100


/*
** 'makewhite' erases all color bits then sets only the current white
** bit
*/
// 'makewhite'擦除所有颜色位，然后只设置当前的白色少量
#define maskcolors	(~(bitmask(BLACKBIT) | WHITEBITS))
// 标记为白色
#define makewhite(g,x)	\
 (x->marked = cast_byte((x->marked & maskcolors) | luaC_white(g)))
// 白色变成灰色
#define white2gray(x)	resetbits(x->marked, WHITEBITS)
// 黑色变成灰色
#define black2gray(x)	resetbit(x->marked, BLACKBIT)

// 值是否为白色
#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define checkdeadkey(n)	lua_assert(!ttisdeadkey(gkey(n)) || ttisnil(gval(n)))


#define checkconsistency(obj)  \
  lua_longassert(!iscollectable(obj) || righttt(obj))

// 
#define markvalue(g,o) { checkconsistency(o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

// 标记一个对象
#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
// 标记一个可以为 NULL 的对象（因为它确实是可选的，
// 或者它被剥离为调试信息，或者在一个未完成的结构中）
#define markobjectN(g,t)	{ if (t) markobject(g,t); }

// 真正的标记一个object
static void reallymarkobject (global_State *g, GCObject *o);


/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** one after last element in a hash array
*/
// 在散列数组中的最后一个元素之后一个
#define gnodelast(h)	gnode(h, cast(size_t, sizenode(h)))


/*
** link collectable object 'o' into list pointed by 'p'
*/
// 将对象o链接到链表p上
#define linkgclist(o,p)	((o)->gclist = (p), (p) = obj2gco(o))


/*
** If key is not marked, mark its entry as dead. This allows key to be
** collected, but keeps its entry in the table.  A dead node is needed
** when Lua looks up for a key (it may be part of a chain) and when
** traversing a weak table (key might be removed from the table during
** traversal). Other places never manipulate dead keys, because its
** associated nil value is enough to signal that the entry is logically
** empty.
*/
// 如果键没有标记，那么条目标记为死。这允许键可以被回收，并且保证该条目还在表中。
// 需要一个死亡节点当lua的查找键（它可能是一条chain的一部分）并且当遍历一个弱表（在遍历表时，键可能从表中移除）
// 其他地方从不操作死键，因为它关联的 nil 值足以表明该条目在逻辑上是空的。
static void removeentry (Node *n) {
  lua_assert(ttisnil(gval(n)));
  if (valiswhite(gkey(n)))
    setdeadvalue(wgkey(n));  /* unused and unmarked key; remove it */
}


/*
** tells whether a key or value can be cleared from a weak
** table. Non-collectable objects are never removed from weak
** tables. Strings behave as 'values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
// 检查一个key或者value是否能从一个弱表中清除。不可收集的对象永远不会从弱表中移除。
// 如果字符串作为值的话，从来不会被移除。对于其他的对象：如果真的收集，不能保持它们；
// 对于最终确定的对象，将它们保存在键中，而不是值中
static int iscleared (global_State *g, const TValue *o) {
  if (!iscollectable(o)) return 0;
  else if (ttisstring(o)) {
    // 标记字符串
    markobject(g, tsvalue(o));  /* strings are 'values', so are never weak */
    return 0;
  }
  // 是否为白色，可以被清除
  else return iswhite(gcvalue(o));
}


/*
** barrier that moves collector forward, that is, mark the white object
** being pointed by a black object. (If in sweep phase, clear the black
** object to white [sweep it] to avoid other barrier calls for this
** same object.)
*/
// 将收集器向前移动的屏障，即标记被一个黑色物体指向的白色物体。
// （如果在扫描阶段，清除黑色object到白色[sweep it]以避免其他barrier调用同一个对象。）

// luaC_barrier_函数说明:
// p为黑色,v为可回收对象,并且v为白色
// 1)若当前GC是需要保持不变式状态(繁殖或原子阶段),则标记v对象;
// 2)否则为清扫阶段,则把p标记为白色,因而说此函数是向前barrier,直接把p跳过清扫时白色切换
// 调用地方:
// 1)lua_copy:若被复制到的位置是C闭包的上值
// 2)lua_setuservalue
// 3)lua_setupvalue:若是为C闭包设置上值
// 4)addk 向函数原型中添加常量时
// 5)lua_setmetatable
// 6)registerlocalvar 向函数原型注册局部变量
// 7)newupvalue 解析时函数原型添加新的上值名
// 8)addprototype 解析时函数原型添加新的内嵌函数原型

void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g))  /* must keep invariant? */
    reallymarkobject(g, v);  /* restore invariant */
  else {  /* sweep phase */
    lua_assert(issweepphase(g));
    makewhite(g, o);  /* mark main obj. as white to avoid other barriers */
  }
}


/*
** barrier that moves collector backward, that is, mark the black object
** pointing to a white object as gray again.
*/
// luaC_barrierback 函数说明 : p为黑色, v为可回收对象, 并且v为白色将p变为灰色并链接到grayagain链表上
//调用地方:
//1)lua_rawset
//2)lua_rawseti
//3)lua_rawsetp
//4)luaH_newkey
//5)luaV_finishset
//6)执行OP_SETLIST
//7)luaV_fastset
void luaC_barrierback_ (lua_State *L, Table *t) {
  global_State *g = G(L);
  lua_assert(isblack(t) && !isdead(g, t));
  black2gray(t);  /* make table gray (again) */
  linkgclist(t, g->grayagain);
}


/*
** barrier for assignments to closed upvalues. Because upvalues are
** shared among closures, it is impossible to know the color of all
** closures pointing to it. So, we assume that the object being assigned
** must be marked.
*/
// 分配给闭合upvalues的barrier。因为upvalues是在各个闭包中共享的，
// 不可能知道指向它的所有的闭包的颜色。所以，我们假定分配的object必须被标记

// luaC_upvalbarrier 函数说明 :
// uv的值是可回收的且是闭合的若是繁殖或原子阶段则标记uv的值调用地方 :
// 1)lua_load
// 2)lua_setupvalue:若是为Lua闭包设置值
// 3)lua_upvaluejoin
// 4)luaF_close
// 5)OP_SETUPVAL

void luaC_upvalbarrier_ (lua_State *L, UpVal *uv) {
  global_State *g = G(L);
  GCObject *o = gcvalue(uv->v);
  lua_assert(!upisopen(uv));  /* ensured by macro luaC_upvalbarrier */
  if (keepinvariant(g))
    markobject(g, o);
}

// 对象o永远不回收
void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(g->allgc == o);  /* object must be 1st in 'allgc' list! */
  // 他们将永远是灰色的
  white2gray(o);  /* they will be gray forever */
  // 从“allgc”列表中删除对象 
  g->allgc = o->next;  /* remove object from 'allgc' list */
  // 将其链接到“fixedgc”列表
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
}


/*
** create a new collectable object (with given type and size) and link
** it to 'allgc' list.
*/
// 创建一个新的GC对象(给定的类型和大小)并且链接在allgc列表中，
GCObject *luaC_newobj (lua_State *L, int tt, size_t sz) {
  global_State *g = G(L);
  // 分配内存
  GCObject *o = cast(GCObject *, luaM_newobject(L, novariant(tt), sz));
  // 将对象置为白色
  o->marked = luaC_white(g);
  // 设置对象的类型
  o->tt = tt;
  // 挂接到gc列表
  o->next = g->allgc;
  g->allgc = o;
  return o;
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/


/*
** mark an object. Userdata, strings, and closed upvalues are visited
** and turned black here. Other objects are marked gray and added
** to appropriate list to be visited (and turned black) later. (Open
** upvalues are already linked in 'headuv' list.)
*/
//   标记一个对象，Userdata, strings和封闭的upvalues访问以后转为黑色，
//   其他类型加入灰色链或其他的辅助标记链
//1) 短、长字符串对象直接标记为黑色
//2) 用户数据对象: 标记元表，将此对象标记为黑色，标记设置的用户对象
//3) Lua闭包和C闭包都添加到gray链表中
//4) 线程添加到grayagain链表中
//5) 函数原型添加到gray链表中
static void reallymarkobject (global_State *g, GCObject *o) {
 reentry:
  white2gray(o);
  switch (o->tt) {
    case LUA_TSHRSTR: {
      gray2black(o);
      g->GCmemtrav += sizelstring(gco2ts(o)->shrlen);
      break;
    }
    case LUA_TLNGSTR: {
      gray2black(o);
      g->GCmemtrav += sizelstring(gco2ts(o)->u.lnglen);
      break;
    }
    case LUA_TUSERDATA: {
      TValue uvalue;
      markobjectN(g, gco2u(o)->metatable);  /* mark its metatable */
      gray2black(o);
      g->GCmemtrav += sizeudata(gco2u(o));
      getuservalue(g->mainthread, gco2u(o), &uvalue);
      if (valiswhite(&uvalue)) {  /* markvalue(g, &uvalue); */
        o = gcvalue(&uvalue);
        goto reentry;
      }
      break;
    }
    case LUA_TLCL: {
      linkgclist(gco2lcl(o), g->gray);
      break;
    }
    case LUA_TCCL: {
      linkgclist(gco2ccl(o), g->gray);
      break;
    }
    case LUA_TTABLE: {
      linkgclist(gco2t(o), g->gray);
      break;
    }
    case LUA_TTHREAD: {
      linkgclist(gco2th(o), g->gray);
      break;
    }
    case LUA_TPROTO: {
      linkgclist(gco2p(o), g->gray);
      break;
    }
    default: lua_assert(0); break;
  }
}


/*
** mark metamethods for basic types
*/
// 标记基本类型的元方法
static void markmt (global_State *g) {
  int i;
  for (i=0; i < LUA_NUMTAGS; i++)
    markobjectN(g, g->mt[i]);
}


/*
** mark all objects in list of being-finalized
*/
static void markbeingfnz (global_State *g) {
  GCObject *o;
  for (o = g->tobefnz; o != NULL; o = o->next)
    markobject(g, o);
}


/*
** Mark all values stored in marked open upvalues from non-marked threads.
** (Values from marked threads were already marked when traversing the
** thread.) Remove from the list threads that no longer have upvalues and
** not-marked threads.
*/
static void remarkupvals (global_State *g) {
  lua_State *thread;
  lua_State **p = &g->twups;
  while ((thread = *p) != NULL) {
    lua_assert(!isblack(thread));  /* threads are never black */
    if (isgray(thread) && thread->openupval != NULL)
      p = &thread->twups;  /* keep marked thread with upvalues in the list */
    else {  /* thread is not marked or without upvalues */
      UpVal *uv;
      *p = thread->twups;  /* remove thread from the list */
      thread->twups = thread;  /* mark that it is out of list */
      for (uv = thread->openupval; uv != NULL; uv = uv->u.open.next) {
        if (uv->u.open.touched) {
          markvalue(g, uv->v);  /* remark upvalue's value */
          uv->u.open.touched = 0;
        }
      }
    }
  }
}


/*
** mark root set and reset all gray lists, to start a new collection
*/
// 标记根集合并重置所有灰名单，以开始新的收集
// 1.将用于辅助标记的各类型对象链表进行初始化清空，其中g->gray是灰色节点链；g->grayagain是需要原子操作标记的灰色节点链；
//   g->weak、g->allweak、g->ephemeron是与弱表相关的链。
// 2.然后依次利用markobject、markvalue、markmt、markbeingfnz标记根(全局)对象:mainthread(主线程(协程), 注册表(registry), 
//   全局元表(metatable), 上次GC循环中剩余的finalize中的对象，并将其加入对应的辅助标记链中。
// 主要是下列操作：
// gray grayagain 链表都置为空
// weak allweak ephemeron 都置为空
// 标记主线程 标记注册表
// 标记基本类型的元表
// 标记上个循环留下来的将要被终结的对象
static void restartcollection (global_State *g) {
  // 清空灰色列表
  g->gray = g->grayagain = NULL;
  // 清空弱表
  g->weak = g->allweak = g->ephemeron = NULL;
  markobject(g, g->mainthread);
  markvalue(g, &g->l_registry);
  markmt(g);
  // 标记上一个循环留下的任何最终确定对象
  markbeingfnz(g);  /* mark any finalizing object left from previous cycle */
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/

/*
** Traverse a table with weak values and link it to proper list. During
** propagate phase, keep it in 'grayagain' list, to be revisited in the
** atomic phase. In the atomic phase, if table has any white value,
** put it in 'weak' list, to be cleared.
*/
// 遍历具有弱值的表并将其链接到正确的链表，在繁殖期间，将其保存在“grayagain”列表中
// 在atomic阶段重新访问。在原子阶段，如果 table 有任何白色值，把它放在“弱”列表中，待清除。
// 遍历强键弱值表
static void traverseweakvalue (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  // 如果有数组部分，假设它可能有白色值（只是为了检查并不值得现在遍历它）
  int hasclears = (h->sizearray > 0);
  // 遍历Hash部分
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    // 检查死键
    checkdeadkey(n);
    // 值是否为空
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else {
      lua_assert(!ttisnil(gkey(n)));
      // 标记值
      markvalue(g, gkey(n));  /* mark key */
      if (!hasclears && iscleared(g, gval(n)))  /* is there a white value? */
        hasclears = 1;  /* table will have to be cleared */
    }
  }
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasclears)
    linkgclist(h, g->weak);  /* has to be cleared later */
}


/*
** Traverse an ephemeron table and link it to proper list. Returns true
** iff any object was marked during this traversal (which implies that
** convergence has to continue). During propagation phase, keep table
** in 'grayagain' list, to be visited again in the atomic phase. In
** the atomic phase, if table has any white->white entry, it has to
** be revisited during ephemeron convergence (as that key may turn
** black). Otherwise, if it has any white key, table has to be cleared
** (in the atomic phase).
*/
// 遍历弱键强值表
static int traverseephemeron (global_State *g, Table *h) {
  int marked = 0;  /* true if an object is marked in this traversal */
  int hasclears = 0;  /* true if table has white keys */
  int hasww = 0;  /* true if table has entry "white-key -> white-value" */
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  /* traverse array part */
  // 遍历数组部分
  for (i = 0; i < h->sizearray; i++) {
    if (valiswhite(&h->array[i])) {
      marked = 1;
      reallymarkobject(g, gcvalue(&h->array[i]));
    }
  }
  /* traverse hash part */
  // 遍历Hash部分
  for (n = gnode(h, 0); n < limit; n++) {
    checkdeadkey(n);
    // value为空
    // 值为空
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    // 键没有标记
    else if (iscleared(g, gkey(n))) {  /* key is not marked (yet)? */
      hasclears = 1;  /* table must be cleared */
      // 值也没有被标记
      if (valiswhite(gval(n)))  /* value not marked yet? */
        hasww = 1;  /* white-white entry */
    }
    // 值没有被标记
    else if (valiswhite(gval(n))) {  /* value not marked yet? */
      marked = 1;
      reallymarkobject(g, gcvalue(gval(n)));  /* mark it now */
    }
  }
  /* link table into proper list */
  // 挂接在合适的链表中
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasww)  /* table has white->white entries? */
    linkgclist(h, g->ephemeron);  /* have to propagate again */
  else if (hasclears)  /* table has white keys? */
    linkgclist(h, g->allweak);  /* may have to clean white keys */
  return marked;
}

// 遍历强表
static void traversestrongtable (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  // 遍历数组部分
  for (i = 0; i < h->sizearray; i++)  /* traverse array part */
    markvalue(g, &h->array[i]);
  // 遍历Hash部分
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    checkdeadkey(n);
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else {
      lua_assert(!ttisnil(gkey(n)));
      // 标记键
      markvalue(g, gkey(n));  /* mark key */
      // 标记值
      markvalue(g, gval(n));  /* mark value */
    }
  }
}

// 遍历表
static lu_mem traversetable (global_State *g, Table *h) {
  const char *weakkey, *weakvalue;
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  // 标记元表
  markobjectN(g, h->metatable);
  if (mode && ttisstring(mode) &&  /* is there a weak mode? */
      ((weakkey = strchr(svalue(mode), 'k')),
       (weakvalue = strchr(svalue(mode), 'v')),
       (weakkey || weakvalue))) {  /* is really weak? */
      // 将表设置为灰色
    black2gray(h);  /* keep table gray */
    // 强键
    if (!weakkey)  /* strong keys? */
      traverseweakvalue(g, h);
    // 强值
    else if (!weakvalue)  /* strong values? */
      traverseephemeron(g, h);
    // 弱键并且弱值表
    else  /* all weak */
        // 直接挂接在allweak链表中
      linkgclist(h, g->allweak);  /* nothing to traverse now */
  }
  else  /* not weak */
    // 遍历强表
    traversestrongtable(g, h);
  return sizeof(Table) + sizeof(TValue) * h->sizearray +
                         sizeof(Node) * cast(size_t, allocsizenode(h));
}


/*
** Traverse a prototype. (While a prototype is being build, its
** arrays can be larger than needed; the extra slots are filled with
** NULL, so the use of 'markobjectN')
*/
// 遍历原型。 （在构建原型时，它的数组可以比需要的大； 额外的插槽充满NULL，所以使用'markobjectN')
static int traverseproto (global_State *g, Proto *f) {
  int i;
  if (f->cache && iswhite(f->cache))
    f->cache = NULL;  /* allow cache to be collected */
  markobjectN(g, f->source);
  for (i = 0; i < f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i = 0; i < f->sizeupvalues; i++)  /* mark upvalue names */
    markobjectN(g, f->upvalues[i].name);
  for (i = 0; i < f->sizep; i++)  /* mark nested protos */
    markobjectN(g, f->p[i]);
  for (i = 0; i < f->sizelocvars; i++)  /* mark local-variable names */
    markobjectN(g, f->locvars[i].varname);
  return sizeof(Proto) + sizeof(Instruction) * f->sizecode +
                         sizeof(Proto *) * f->sizep +
                         sizeof(TValue) * f->sizek +
                         sizeof(int) * f->sizelineinfo +
                         sizeof(LocVar) * f->sizelocvars +
                         sizeof(Upvaldesc) * f->sizeupvalues;
}

// 遍历C闭包
static lu_mem traverseCclosure (global_State *g, CClosure *cl) {
  int i;
  // 标记所有的upvalue
  for (i = 0; i < cl->nupvalues; i++)  /* mark its upvalues */
    markvalue(g, &cl->upvalue[i]);
  return sizeCclosure(cl->nupvalues);
}

/*
** open upvalues point to values in a thread, so those values should
** be marked when the thread is traversed except in the atomic phase
** (because then the value cannot be changed by the thread and the
** thread may not be traversed again)
*/
// 遍历Lua闭包
static lu_mem traverseLclosure (global_State *g, LClosure *cl) {
  int i;
  // 标记原型
  markobjectN(g, cl->p);  /* mark its prototype */
  // 标记upvalues
  for (i = 0; i < cl->nupvalues; i++) {  /* mark its upvalues */
    UpVal *uv = cl->upvals[i];
    if (uv != NULL) {
      if (upisopen(uv) && g->gcstate != GCSinsideatomic)
        uv->u.open.touched = 1;  /* can be marked in 'remarkupvals' */
      else
        markvalue(g, uv->v);
    }
  }
  return sizeLclosure(cl->nupvalues);
}

// 遍历线程
static lu_mem traversethread (global_State *g, lua_State *th) {
  StkId o = th->stack;
  if (o == NULL)
    return 1;  /* stack not completely built yet */
  lua_assert(g->gcstate == GCSinsideatomic ||
             th->openupval == NULL || isintwups(th));
  // 标志栈上面的所有元素
  for (; o < th->top; o++)  /* mark live elements in the stack */
    markvalue(g, o);
  // 最后的阶段
  if (g->gcstate == GCSinsideatomic) {  /* final traversal? */
    // 栈的真实结尾
    StkId lim = th->stack + th->stacksize;  /* real end of stack */
    // 将栈顶和结尾之间的值清除
    for (; o < lim; o++)  /* clear not-marked stack slice */
      setnilvalue(o);
    /* 'remarkupvals' may have removed thread from 'twups' list */
    // 'remarkupvals'可能已经从 'twups' 列表中删除了线程
    if (!isintwups(th) && th->openupval != NULL) {
      th->twups = g->twups;  /* link it back to the list */
      g->twups = th;
    }
  }
  else if (g->gckind != KGC_EMERGENCY)
    luaD_shrinkstack(th); /* do not change stack in emergency cycle */
  return (sizeof(lua_State) + sizeof(TValue) * th->stacksize +
          sizeof(CallInfo) * th->nci);
}


/*
** traverse one gray object, turning it to black (except for threads,
** which are always gray).
*/
// 遍历一个灰色物体，把它变成黑色（除了线程，总是灰色的）。
static void propagatemark (global_State *g) {
  lu_mem size;
  // 从灰色链表中取出一个对象
  GCObject *o = g->gray;
  lua_assert(isgray(o));
  // 将其变成黑色
  gray2black(o);
  switch (o->tt) {
    case LUA_TTABLE: {
      Table *h = gco2t(o);
      // 从gray链表中移除
      g->gray = h->gclist;  /* remove from 'gray' list */
      // 遍历表
      size = traversetable(g, h);
      break;
    }
    case LUA_TLCL: {
      LClosure *cl = gco2lcl(o);
      // 从gray链表中移除
      g->gray = cl->gclist;  /* remove from 'gray' list */
      // 遍历Lua闭包
      size = traverseLclosure(g, cl);
      break;
    }
    case LUA_TCCL: {
      CClosure *cl = gco2ccl(o);
      // 从gray链表中移除
      g->gray = cl->gclist;  /* remove from 'gray' list */
      // 遍历C闭包
      size = traverseCclosure(g, cl);
      break;
    }
    case LUA_TTHREAD: {
      lua_State *th = gco2th(o);
      // 从gray链表中移除
      g->gray = th->gclist;  /* remove from 'gray' list */
      // 插入到grayagain链表中
      linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
      // 变成灰色
      black2gray(o);
      // 遍历线程
      size = traversethread(g, th);
      break;
    }
    case LUA_TPROTO: {
      Proto *p = gco2p(o);
      // 从gray链表中移除
      g->gray = p->gclist;  /* remove from 'gray' list */
      // 遍历函数原型
      size = traverseproto(g, p);
      break;
    }
    default: lua_assert(0); return;
  }
  g->GCmemtrav += size;
}


// 标记所有的灰色
static void propagateall (global_State *g) {
  while (g->gray) propagatemark(g);
}


static void convergeephemerons (global_State *g) {
  int changed;
  do {
    GCObject *w;
    GCObject *next = g->ephemeron;  /* get ephemeron list */
    g->ephemeron = NULL;  /* tables may return to this list when traversed */
    changed = 0;
    while ((w = next) != NULL) {
      next = gco2t(w)->gclist;
      if (traverseephemeron(g, gco2t(w))) {  /* traverse marked some value? */
        propagateall(g);  /* propagate changes */
        changed = 1;  /* will have to revisit all ephemeron tables */
      }
    }
  } while (changed);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/


/*
** clear entries with unmarked keys from all weaktables in list 'l' up
** to element 'f'
*/
static void clearkeys (global_State *g, GCObject *l, GCObject *f) {
  for (; l != f; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *n, *limit = gnodelast(h);
    for (n = gnode(h, 0); n < limit; n++) {
      if (!ttisnil(gval(n)) && (iscleared(g, gkey(n)))) {
        setnilvalue(gval(n));  /* remove value ... */
      }
      if (ttisnil(gval(n)))  /* is entry empty? */
        removeentry(n);  /* remove entry from table */
    }
  }
}


/*
** clear entries with unmarked values from all weaktables in list 'l' up
** to element 'f'
*/
static void clearvalues (global_State *g, GCObject *l, GCObject *f) {
  for (; l != f; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *n, *limit = gnodelast(h);
    unsigned int i;
    for (i = 0; i < h->sizearray; i++) {
      TValue *o = &h->array[i];
      if (iscleared(g, o))  /* value was collected? */
        setnilvalue(o);  /* remove value */
    }
    for (n = gnode(h, 0); n < limit; n++) {
      if (!ttisnil(gval(n)) && iscleared(g, gval(n))) {
        setnilvalue(gval(n));  /* remove value ... */
        removeentry(n);  /* and remove entry from table */
      }
    }
  }
}


void luaC_upvdeccount (lua_State *L, UpVal *uv) {
  lua_assert(uv->refcount > 0);
  uv->refcount--;
  if (uv->refcount == 0 && !upisopen(uv))
    luaM_free(L, uv);
}


static void freeLclosure (lua_State *L, LClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++) {
    UpVal *uv = cl->upvals[i];
    if (uv)
      luaC_upvdeccount(L, uv);
  }
  luaM_freemem(L, cl, sizeLclosure(cl->nupvalues));
}


static void freeobj (lua_State *L, GCObject *o) {
  switch (o->tt) {
    case LUA_TPROTO: luaF_freeproto(L, gco2p(o)); break;
    case LUA_TLCL: {
      freeLclosure(L, gco2lcl(o));
      break;
    }
    case LUA_TCCL: {
      luaM_freemem(L, o, sizeCclosure(gco2ccl(o)->nupvalues));
      break;
    }
    case LUA_TTABLE: luaH_free(L, gco2t(o)); break;
    case LUA_TTHREAD: luaE_freethread(L, gco2th(o)); break;
    case LUA_TUSERDATA: luaM_freemem(L, o, sizeudata(gco2u(o))); break;
    case LUA_TSHRSTR:
      luaS_remove(L, gco2ts(o));  /* remove it from hash table */
      luaM_freemem(L, o, sizelstring(gco2ts(o)->shrlen));
      break;
    case LUA_TLNGSTR: {
      luaM_freemem(L, o, sizelstring(gco2ts(o)->u.lnglen));
      break;
    }
    default: lua_assert(0);
  }
}


#define sweepwholelist(L,p)	sweeplist(L,p,MAX_LUMEM)
static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count);


/*
** sweep at most 'count' elements from a list of GCObjects erasing dead
** objects, where a dead object is one marked with the old (non current)
** white; change all non-dead objects back to white, preparing for next
** collection cycle. Return where to continue the traversal or NULL if
** list is finished.
*/
static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count) {
  global_State *g = G(L);
  int ow = otherwhite(g);
  int white = luaC_white(g);  /* current white */
  while (*p != NULL && count-- > 0) {
    GCObject *curr = *p;
    int marked = curr->marked;
    if (isdeadm(ow, marked)) {  /* is 'curr' dead? */
      *p = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr);  /* erase 'curr' */
    }
    else {  /* change mark to 'white' */
      curr->marked = cast_byte((marked & maskcolors) | white);
      p = &curr->next;  /* go to next element */
    }
  }
  return (*p == NULL) ? NULL : p;
}


/*
** sweep a list until a live object (or end of list)
*/
static GCObject **sweeptolive (lua_State *L, GCObject **p) {
  GCObject **old = p;
  do {
    p = sweeplist(L, p, 1);
  } while (p == old);
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

/*
** If possible, shrink string table
*/
static void checkSizes (lua_State *L, global_State *g) {
  if (g->gckind != KGC_EMERGENCY) {
    l_mem olddebt = g->GCdebt;
    if (g->strt.nuse < g->strt.size / 4)  /* string table too big? */
      luaS_resize(L, g->strt.size / 2);  /* shrink it a little */
    g->GCestimate += g->GCdebt - olddebt;  /* update estimate */
  }
}

// 从tobefnz链表头取下来第一个元素，挂接在allgc链表表头，然后返回
static GCObject *udata2finalize (global_State *g) {
  // 把tobefnz的第一个取下来
  GCObject *o = g->tobefnz;  /* get first element */
  lua_assert(tofinalize(o));
  g->tobefnz = o->next;  /* remove it from 'tobefnz' list */
  // 挂接到allgc链表上去
  o->next = g->allgc;  /* return it to 'allgc' list */
  g->allgc = o;
  // 重置可回收标记
  resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
  // 如果是清扫阶段，将其置为白色
  if (issweepphase(g))
    makewhite(g, o);  /* "sweep" object */
  return o;
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_callnoyield(L, L->top - 2, 0);
}


static void GCTM (lua_State *L, int propagateerrors) {
  global_State *g = G(L);
  const TValue *tm;
  TValue v;
  // udata2finalize从tobefnz链表头取下第一个链表对象，设置v
  setgcovalue(L, &v, udata2finalize(g));
  // 得到TM_GC的元表
  tm = luaT_gettmbyobj(L, &v, TM_GC);
  // 是否存在
  if (tm != NULL && ttisfunction(tm)) {  /* is there a finalizer? */
    int status;
    lu_byte oldah = L->allowhook;
    int running  = g->gcrunning;
    L->allowhook = 0;  /* stop debug hooks during GC metamethod */
    g->gcrunning = 0;  /* avoid GC steps */
    // 将函数和GC的对象入栈
    setobj2s(L, L->top, tm);  /* push finalizer... */
    setobj2s(L, L->top + 1, &v);  /* ... and its argument */
    L->top += 2;  /* and (next line) call the finalizer */
    // 调用状态增加终结器状态
    L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
    // 调用函数
    status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0);
    // 恢复状态
    L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
    L->allowhook = oldah;  /* restore hooks */
    g->gcrunning = running;  /* restore state */
    // 调用gc时出错
    if (status != LUA_OK && propagateerrors) {  /* error while running __gc? */
      if (status == LUA_ERRRUN) {  /* is there an error object? */
        // 是否有出错信息
        const char *msg = (ttisstring(L->top - 1))
                            ? svalue(L->top - 1)
                            : "no message";
        luaO_pushfstring(L, "error in __gc metamethod (%s)", msg);
        status = LUA_ERRGCMM;  /* error in __gc metamethod */
      }
      // 抛出错误
      luaD_throw(L, status);  /* re-throw error */
    }
  }
}


/*
** call a few (up to 'g->gcfinnum') finalizers
*/
// 调用最多gcfinnum个终结器
static int runafewfinalizers (lua_State *L) {
  global_State *g = G(L);
  unsigned int i;
  lua_assert(!g->tobefnz || g->gcfinnum > 0);
  // 最多调用g->gcfinnum从链表g->tobefnz中取出的元素
  for (i = 0; g->tobefnz && i < g->gcfinnum; i++)
    GCTM(L, 1);  /* call one finalizer */
  // 每次都会增加一倍的数量
  g->gcfinnum = (!g->tobefnz) ? 0  /* nothing more to finalize? */
                    : g->gcfinnum * 2;  /* else call a few more next time */
  return i;
}


/*
** call all pending finalizers
*/
static void callallpendingfinalizers (lua_State *L) {
  global_State *g = G(L);
  while (g->tobefnz)
    GCTM(L, 0);
}


/*
** find last 'next' field in list 'p' list (to add elements in its end)
*/
static GCObject **findlast (GCObject **p) {
  while (*p != NULL)
    p = &(*p)->next;
  return p;
}


/*
** move all unreachable objects (or 'all' objects) that need
** finalization from list 'finobj' to list 'tobefnz' (to be finalized)
*/
static void separatetobefnz (global_State *g, int all) {
  GCObject *curr;
  GCObject **p = &g->finobj;
  GCObject **lastnext = findlast(&g->tobefnz);
  while ((curr = *p) != NULL) {  /* traverse all finalizable objects */
    lua_assert(tofinalize(curr));
    if (!(iswhite(curr) || all))  /* not being collected? */
      p = &curr->next;  /* don't bother with it */
    else {
      *p = curr->next;  /* remove 'curr' from 'finobj' list */
      curr->next = *lastnext;  /* link at the end of 'tobefnz' list */
      *lastnext = curr;
      lastnext = &curr->next;
    }
  }
}


/*
** if object 'o' has a finalizer, remove it from 'allgc' list (must
** search the list to find it) and link it in 'finobj' list.
*/
void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt) {
  global_State *g = G(L);
  if (tofinalize(o) ||                 /* obj. is already marked... */
      gfasttm(g, mt, TM_GC) == NULL)   /* or has no finalizer? */
    return;  /* nothing to be done */
  else {  /* move 'o' to 'finobj' list */
    GCObject **p;
    if (issweepphase(g)) {
      makewhite(g, o);  /* "sweep" object 'o' */
      if (g->sweepgc == &o->next)  /* should not remove 'sweepgc' object */
        g->sweepgc = sweeptolive(L, g->sweepgc);  /* change 'sweepgc' */
    }
    /* search for pointer pointing to 'o' */
    for (p = &g->allgc; *p != o; p = &(*p)->next) { /* empty */ }
    *p = o->next;  /* remove 'o' from 'allgc' list */
    o->next = g->finobj;  /* link it in 'finobj' list */
    g->finobj = o;
    l_setbit(o->marked, FINALIZEDBIT);  /* mark it as such */
  }
}

/* }====================================================== */



/*
** {======================================================
** GC control
** =======================================================
*/


/*
** Set a reasonable "time" to wait before starting a new GC cycle; cycle
** will start when memory use hits threshold. (Division by 'estimate'
** should be OK: it cannot be zero (because Lua cannot even start with
** less than PAUSEADJ bytes).
*/
static void setpause (global_State *g) {
  l_mem threshold, debt;
  // GCestimate可以理解为lua的实际占用内存
  // 当GC循环执行到GCScallfin状态以前,g->GCestimate与gettotalbytes(g)必然相等，即可以将GCestimate理解为当前lua的实际占用内存
  // 而MAX_LMEM/estimate即为本机最大内存量与当前lua实际使用量的比值
  l_mem estimate = g->GCestimate / PAUSEADJ;  /* adjust 'estimate' */
  lua_assert(estimate > 0);
  // 而threshold即为之前文章中提到内存的阀值。该阀值大部分时间是通过estimate*gcpause得到的。gcpause默认值为100。
  // 当然gcpause这个值也是可以通过手动GC函数collectgarbage(“setpause”)来设定的，当gcpause为200时，意味着，
  // threshold = 2GCestimate，则debt = -GCestimate(gettotalbytes约等于GCestimate)，
  // 所以GCdebt将在内存分配器分配新内存时由-GCestimate缓慢增长到大于零之后再开始新的一轮GC，所以pause被称为“间歇率”，
  // 即将pause设定为200时就会让收集器等到总内存使用量达到之前的两倍时才开始新的GC循环。
  threshold = (g->gcpause < MAX_LMEM / estimate)  /* overflow? */
            ? estimate * g->gcpause  /* no overflow */
            : MAX_LMEM;  /* overflow; truncate to maximum */
  debt = gettotalbytes(g) - threshold;
  luaE_setdebt(g, debt);
}


/*
** Enter first sweep phase.
** The call to 'sweeplist' tries to make pointer point to an object
** inside the list (instead of to the header), so that the real sweep do
** not need to skip objects created between "now" and the start of the
** real sweep.
*/
// 进入第一个扫描阶段。
static void entersweep (lua_State *L) {
  global_State *g = G(L);
  g->gcstate = GCSswpallgc;
  lua_assert(g->sweepgc == NULL);
  g->sweepgc = sweeplist(L, &g->allgc, 1);
}


void luaC_freeallobjects (lua_State *L) {
  global_State *g = G(L);
  separatetobefnz(g, 1);  /* separate all objects with finalizers */
  lua_assert(g->finobj == NULL);
  callallpendingfinalizers(L);
  lua_assert(g->tobefnz == NULL);
  g->currentwhite = WHITEBITS; /* this "white" makes all objects look dead */
  g->gckind = KGC_NORMAL;
  sweepwholelist(L, &g->finobj);
  sweepwholelist(L, &g->allgc);
  sweepwholelist(L, &g->fixedgc);  /* collect fixed objects */
  lua_assert(g->strt.nuse == 0);
}


static l_mem atomic (lua_State *L) {
  global_State *g = G(L);
  l_mem work;
  GCObject *origweak, *origall;
  GCObject *grayagain = g->grayagain;  /* save original list */
  lua_assert(g->ephemeron == NULL && g->weak == NULL);
  lua_assert(!iswhite(g->mainthread));
  g->gcstate = GCSinsideatomic;
  g->GCmemtrav = 0;  /* start counting work */
  markobject(g, L);  /* mark running thread */
  /* registry and global metatables may be changed by API */
  markvalue(g, &g->l_registry);
  // 标记全局元表
  markmt(g);  /* mark global metatables */
  /* remark occasional upvalues of (maybe) dead threads */
  // 标记偶尔的死线程的upvalues
  remarkupvals(g);
  // 
  propagateall(g);  /* propagate changes */
  work = g->GCmemtrav;  /* stop counting (do not recount 'grayagain') */
  g->gray = grayagain;
  propagateall(g);  /* traverse 'grayagain' list */
  g->GCmemtrav = 0;  /* restart counting */
  convergeephemerons(g);
  /* at this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers */
  clearvalues(g, g->weak, NULL);
  clearvalues(g, g->allweak, NULL);
  origweak = g->weak; origall = g->allweak;
  work += g->GCmemtrav;  /* stop counting (objects being finalized) */
  separatetobefnz(g, 0);  /* separate objects to be finalized */
  g->gcfinnum = 1;  /* there may be objects to be finalized */
  markbeingfnz(g);  /* mark objects that will be finalized */
  propagateall(g);  /* remark, to propagate 'resurrection' */
  g->GCmemtrav = 0;  /* restart counting */
  convergeephemerons(g);
  /* at this point, all resurrected objects are marked. */
  /* remove dead objects from weak tables */
  clearkeys(g, g->ephemeron, NULL);  /* clear keys from all ephemeron tables */
  clearkeys(g, g->allweak, NULL);  /* clear keys from all 'allweak' tables */
  /* clear values from resurrected weak tables */
  clearvalues(g, g->weak, origweak);
  clearvalues(g, g->allweak, origall);
  luaS_clearcache(g);
  g->currentwhite = cast_byte(otherwhite(g));  /* flip current white */
  work += g->GCmemtrav;  /* complete counting */
  return work;  /* estimate of memory marked by 'atomic' */
}


static lu_mem sweepstep (lua_State *L, global_State *g,
                         int nextstate, GCObject **nextlist) {
  if (g->sweepgc) {
    l_mem olddebt = g->GCdebt;
    // 每次清理GCSWEEPMAX的数量
    g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX);
    g->GCestimate += g->GCdebt - olddebt;  /* update estimate */
    if (g->sweepgc)  /* is there still something to sweep? */
      return (GCSWEEPMAX * GCSWEEPCOST);
  }
  /* else enter next state */
  g->gcstate = nextstate;
  g->sweepgc = nextlist;
  return 0;
}

// singlestep实际上就是一个简单的状态机。
static lu_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  switch (g->gcstate) {
      // 从根对象开始标记，将白色置为灰色，并加入到灰色链表中
      // 暂停阶段
    case GCSpause: {
      g->GCmemtrav = g->strt.size * sizeof(GCObject*);
      // 重启收集
      restartcollection(g);
      g->gcstate = GCSpropagate;
      return g->GCmemtrav;
    }
    // 繁殖阶段
    case GCSpropagate: {
      g->GCmemtrav = 0;
      lua_assert(g->gray);
      propagatemark(g);
      // 只有没有灰色的Objects，才改变状态
       if (g->gray == NULL)  /* no more gray objects? */
        g->gcstate = GCSatomic;  /* finish propagate phase */
      return g->GCmemtrav;  /* memory traversed in this step */
    }
    // 最后对灰色链表进行一次清除且保证是原子操作。
	// 1重新遍历(跟踪)根对象。
    // 2遍历之前的grayagain(grayagain上会有弱表的存在), 并清理弱表的空间。
	// 3调用separatetobefnz函数将带__gc函数的需要回收的(白色)对象放到global_State.tobefnz表中, 留待以后清理。
	// 4.使global_State.tobefnz上的所有对象全部可达。
	// 5.将当前白色值切换到新一轮的白色值。
    case GCSatomic: {
      lu_mem work;
      // 保证灰色链表为空
      propagateall(g);  /* make sure gray list is empty */
      // 原子遍历
      work = atomic(L);  /* work is what was traversed by 'atomic' */
      entersweep(L);
      g->GCestimate = gettotalbytes(g);  /* first estimate */;
      return work;
    }
    // 据不同类型的对象，进行分步回收。回收中遍历不同类型对象的存储链表
    // GCSswpallgc将通过sweepstep将g->allgc上的所有死对象释放（GCSatomic状态以前的白色值的对象)，并将活对象重新标记为当前白色值。
    case GCSswpallgc: {  /* sweep "regular" objects */
      return sweepstep(L, g, GCSswpfinobj, &g->finobj);
    }
    // GCSswpfinobj和GCSswptobefnz两个状态也调用了sweepstep函数。但是g->finobj和g->tobefnz链表上是不可能有死对象的，
    // 因此它们的作用仅仅是将这些对象重新设置为新一轮的白色。
    // 该对象存储链表是否到达链尾
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      return sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
    }
    // 逐个判断对象颜色是否为白
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      return sweepstep(L, g, GCSswpend, NULL);
    }
    // 释放对象所占用的空间
    case GCSswpend: {  /* finish sweeps */
      makewhite(g, g->mainthread);  /* sweep main thread */
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      return 0;
    }
    // 将对象颜色置为白
    case GCScallfin: {  /* call remaining finalizers */
      if (g->tobefnz && g->gckind != KGC_EMERGENCY) {
        int n = runafewfinalizers(L);
        return (n * GCFINALIZECOST);
      }
      else {  /* emergency mode or no more finalizers */
        g->gcstate = GCSpause;  /* finish collection */
        return 0;
      }
    }
    default: lua_assert(0); return 0;
  }
}


/*
** advances the garbage collector until it reaches a state allowed
** by 'statemask'
*/
void luaC_runtilstate (lua_State *L, int statesmask) {
  global_State *g = G(L);
  while (!testbit(statesmask, g->gcstate))
    singlestep(L);
}


/*
** get GC debt and convert it from Kb to 'work units' (avoid zero debt
** and overflows)
*/
// 得到GCdebt并且将其从Kb转换到工作单元(防止零debt和溢出)
// 就是计算一次需要遍历的字节数
static l_mem getdebt (global_State *g) {
  l_mem debt = g->GCdebt;
  int stepmul = g->gcstepmul;
  if (debt <= 0) return 0;  /* minimal debt */
  else {
    debt = (debt / STEPMULADJ) + 1;
    // 计算本次单步gc需要遍历的对象字节数: debt = (debt / STEPMULADJ + 1) * gcstepmul 
    debt = (debt < MAX_LMEM / stepmul) ? debt * stepmul : MAX_LMEM;
    return debt;
  }
}

/*
** performs a basic GC step when collector is running
*/
// 当收集器运行时执行基本的 GC 步骤
 // 在luaC_step这个函数中，debt并非是GCdebt而是被乘以倍率的GCdebt。这个倍率即为gcstepmul。
void luaC_step (lua_State *L) {
  global_State *g = G(L);
 
  // 计算本次单步gc需要遍历的对象字节数
  l_mem debt = getdebt(g);  /* GC deficit (be paid now) */
  if (!g->gcrunning) {  /* not running? */
    luaE_setdebt(g, -GCSTEPSIZE * 10);  /* avoid being called too often */
    return;
  }
  // 当GCdebt大于零时，luaC_step会通过控制GCdebt，循环调用singlestep
  // 重复直到暂停或者足够的信用(负的debt)
  // 将GCdebt放大后的debt将会导致该循环的次数增加，从而延长”一步”的工作量，所以stepmul被称为“步进倍率”。
  // 如果将stepmul设定的很大，则将会将GCdebt放大很多倍，那么GC将会退化成之前的GC版本stop-the-world 
  // 因为它试图在尽可能多的回收内存，导致阻塞。在这个循环中，将会调用singlestep，进行GC的分步过程
  do {  /* repeat until pause or enough "credit" (negative debt) */
      // 执行一步
    lu_mem work = singlestep(L);  /* perform one single step */
    debt -= work;
  } while (debt > -GCSTEPSIZE && g->gcstate != GCSpause);
  if (g->gcstate == GCSpause)
    setpause(g);  /* pause until next cycle */
  else {
    // 将工作单元转换成内存大小
    debt = (debt / g->gcstepmul) * STEPMULADJ;  /* convert 'work units' to Kb */
    // 设置新的debt的数量
    luaE_setdebt(g, debt);
    runafewfinalizers(L);
  }
}


/*
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
void luaC_fullgc (lua_State *L, int isemergency) {
  global_State *g = G(L);
  lua_assert(g->gckind == KGC_NORMAL);
  if (isemergency) g->gckind = KGC_EMERGENCY;  /* set flag */
  if (keepinvariant(g)) {  /* black objects? */
    entersweep(L); /* sweep everything to turn them back to white */
  }
  /* finish any pending sweep phase to start a new cycle */
  luaC_runtilstate(L, bitmask(GCSpause));
  luaC_runtilstate(L, ~bitmask(GCSpause));  /* start new collection */
  luaC_runtilstate(L, bitmask(GCScallfin));  /* run up to finalizers */
  /* estimate must be correct after a full GC cycle */
  lua_assert(g->GCestimate == gettotalbytes(g));
  luaC_runtilstate(L, bitmask(GCSpause));  /* finish collection */
  g->gckind = KGC_NORMAL;
  setpause(g);
}

/* }====================================================== */


