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
// ɨ��һ��Ԫ�صĳɱ���һ��С����Ĵ�С����ͨ��һЩɨ���ٶȵĵ�����
#define GCSWEEPCOST	((sizeof(TString) + 4) / 4)

/* maximum number of elements to sweep in each single step */
// ÿһ����Ҫɨ������Ԫ����
#define GCSWEEPMAX	(cast_int((GCSTEPSIZE / GCSWEEPCOST) / 4))

/* cost of calling one finalizer */
// ����һ��finalizer�ĳɱ�
#define GCFINALIZECOST	GCSWEEPCOST


/*
** macro to adjust 'stepmul': 'stepmul' is actually used like
** 'stepmul / STEPMULADJ' (value chosen by tests)
*/
// ���ڵ���stepmul�ĺ꣬stepmulʵ��������stepmul/STEPMULADJ
#define STEPMULADJ		200


/*
** macro to adjust 'pause': 'pause' is actually used like
** 'pause / PAUSEADJ' (value chosen by tests)
*/
// ���ڵ���pause�ĺ꣺pauseʵ��������pause/PAUSEADJ
#define PAUSEADJ		100


/*
** 'makewhite' erases all color bits then sets only the current white
** bit
*/
// 'makewhite'����������ɫλ��Ȼ��ֻ���õ�ǰ�İ�ɫ����
#define maskcolors	(~(bitmask(BLACKBIT) | WHITEBITS))
// ���Ϊ��ɫ
#define makewhite(g,x)	\
 (x->marked = cast_byte((x->marked & maskcolors) | luaC_white(g)))
// ��ɫ��ɻ�ɫ
#define white2gray(x)	resetbits(x->marked, WHITEBITS)
// ��ɫ��ɻ�ɫ
#define black2gray(x)	resetbit(x->marked, BLACKBIT)

// ֵ�Ƿ�Ϊ��ɫ
#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define checkdeadkey(n)	lua_assert(!ttisdeadkey(gkey(n)) || ttisnil(gval(n)))


#define checkconsistency(obj)  \
  lua_longassert(!iscollectable(obj) || righttt(obj))

// 
#define markvalue(g,o) { checkconsistency(o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

// ���һ������
#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
// ���һ������Ϊ NULL �Ķ�����Ϊ��ȷʵ�ǿ�ѡ�ģ�
// ������������Ϊ������Ϣ��������һ��δ��ɵĽṹ�У�
#define markobjectN(g,t)	{ if (t) markobject(g,t); }

// �����ı��һ��object
static void reallymarkobject (global_State *g, GCObject *o);


/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** one after last element in a hash array
*/
// ��ɢ�������е����һ��Ԫ��֮��һ��
#define gnodelast(h)	gnode(h, cast(size_t, sizenode(h)))


/*
** link collectable object 'o' into list pointed by 'p'
*/
// ������o���ӵ�����p��
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
// �����û�б�ǣ���ô��Ŀ���Ϊ��������������Ա����գ����ұ�֤����Ŀ���ڱ��С�
// ��Ҫһ�������ڵ㵱lua�Ĳ��Ҽ�����������һ��chain��һ���֣����ҵ�����һ�������ڱ�����ʱ�������ܴӱ����Ƴ���
// �����ط��Ӳ�������������Ϊ�������� nil ֵ���Ա�������Ŀ���߼����ǿյġ�
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
// ���һ��key����value�Ƿ��ܴ�һ������������������ռ��Ķ�����Զ������������Ƴ���
// ����ַ�����Ϊֵ�Ļ����������ᱻ�Ƴ������������Ķ����������ռ������ܱ������ǣ�
// ��������ȷ���Ķ��󣬽����Ǳ����ڼ��У�������ֵ��
static int iscleared (global_State *g, const TValue *o) {
  if (!iscollectable(o)) return 0;
  else if (ttisstring(o)) {
    // ����ַ���
    markobject(g, tsvalue(o));  /* strings are 'values', so are never weak */
    return 0;
  }
  // �Ƿ�Ϊ��ɫ�����Ա����
  else return iswhite(gcvalue(o));
}


/*
** barrier that moves collector forward, that is, mark the white object
** being pointed by a black object. (If in sweep phase, clear the black
** object to white [sweep it] to avoid other barrier calls for this
** same object.)
*/
// ���ռ�����ǰ�ƶ������ϣ�����Ǳ�һ����ɫ����ָ��İ�ɫ���塣
// �������ɨ��׶Σ������ɫobject����ɫ[sweep it]�Ա�������barrier����ͬһ�����󡣣�

// luaC_barrier_����˵��:
// pΪ��ɫ,vΪ�ɻ��ն���,����vΪ��ɫ
// 1)����ǰGC����Ҫ���ֲ���ʽ״̬(��ֳ��ԭ�ӽ׶�),����v����;
// 2)����Ϊ��ɨ�׶�,���p���Ϊ��ɫ,���˵�˺�������ǰbarrier,ֱ�Ӱ�p������ɨʱ��ɫ�л�
// ���õط�:
// 1)lua_copy:�������Ƶ���λ����C�հ�����ֵ
// 2)lua_setuservalue
// 3)lua_setupvalue:����ΪC�հ�������ֵ
// 4)addk ����ԭ������ӳ���ʱ
// 5)lua_setmetatable
// 6)registerlocalvar ����ԭ��ע��ֲ�����
// 7)newupvalue ����ʱ����ԭ������µ���ֵ��
// 8)addprototype ����ʱ����ԭ������µ���Ƕ����ԭ��

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
// luaC_barrierback ����˵�� : pΪ��ɫ, vΪ�ɻ��ն���, ����vΪ��ɫ��p��Ϊ��ɫ�����ӵ�grayagain������
//���õط�:
//1)lua_rawset
//2)lua_rawseti
//3)lua_rawsetp
//4)luaH_newkey
//5)luaV_finishset
//6)ִ��OP_SETLIST
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
// ������պ�upvalues��barrier����Ϊupvalues���ڸ����հ��й���ģ�
// ������֪��ָ���������еıհ�����ɫ�����ԣ����Ǽٶ������object���뱻���

// luaC_upvalbarrier ����˵�� :
// uv��ֵ�ǿɻ��յ����Ǳպϵ����Ƿ�ֳ��ԭ�ӽ׶�����uv��ֵ���õط� :
// 1)lua_load
// 2)lua_setupvalue:����ΪLua�հ�����ֵ
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

// ����o��Զ������
void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(g->allgc == o);  /* object must be 1st in 'allgc' list! */
  // ���ǽ���Զ�ǻ�ɫ��
  white2gray(o);  /* they will be gray forever */
  // �ӡ�allgc���б���ɾ������ 
  g->allgc = o->next;  /* remove object from 'allgc' list */
  // �������ӵ���fixedgc���б�
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
}


/*
** create a new collectable object (with given type and size) and link
** it to 'allgc' list.
*/
// ����һ���µ�GC����(���������ͺʹ�С)����������allgc�б��У�
GCObject *luaC_newobj (lua_State *L, int tt, size_t sz) {
  global_State *g = G(L);
  // �����ڴ�
  GCObject *o = cast(GCObject *, luaM_newobject(L, novariant(tt), sz));
  // ��������Ϊ��ɫ
  o->marked = luaC_white(g);
  // ���ö��������
  o->tt = tt;
  // �ҽӵ�gc�б�
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
//   ���һ������Userdata, strings�ͷ�յ�upvalues�����Ժ�תΪ��ɫ��
//   �������ͼ����ɫ���������ĸ��������
//1) �̡����ַ�������ֱ�ӱ��Ϊ��ɫ
//2) �û����ݶ���: ���Ԫ�����˶�����Ϊ��ɫ��������õ��û�����
//3) Lua�հ���C�հ�����ӵ�gray������
//4) �߳���ӵ�grayagain������
//5) ����ԭ����ӵ�gray������
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
// ��ǻ������͵�Ԫ����
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
// ��Ǹ����ϲ��������л��������Կ�ʼ�µ��ռ�
// 1.�����ڸ�����ǵĸ����Ͷ���������г�ʼ����գ�����g->gray�ǻ�ɫ�ڵ�����g->grayagain����Ҫԭ�Ӳ�����ǵĻ�ɫ�ڵ�����
//   g->weak��g->allweak��g->ephemeron����������ص�����
// 2.Ȼ����������markobject��markvalue��markmt��markbeingfnz��Ǹ�(ȫ��)����:mainthread(���߳�(Э��), ע���(registry), 
//   ȫ��Ԫ��(metatable), �ϴ�GCѭ����ʣ���finalize�еĶ��󣬲���������Ӧ�ĸ���������С�
// ��Ҫ�����в�����
// gray grayagain ������Ϊ��
// weak allweak ephemeron ����Ϊ��
// ������߳� ���ע���
// ��ǻ������͵�Ԫ��
// ����ϸ�ѭ���������Ľ�Ҫ���ս�Ķ���
static void restartcollection (global_State *g) {
  // ��ջ�ɫ�б�
  g->gray = g->grayagain = NULL;
  // �������
  g->weak = g->allweak = g->ephemeron = NULL;
  markobject(g, g->mainthread);
  markvalue(g, &g->l_registry);
  markmt(g);
  // �����һ��ѭ�����µ��κ�����ȷ������
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
// ����������ֵ�ı��������ӵ���ȷ�������ڷ�ֳ�ڼ䣬���䱣���ڡ�grayagain���б���
// ��atomic�׶����·��ʡ���ԭ�ӽ׶Σ���� table ���κΰ�ɫֵ���������ڡ������б��У��������
// ����ǿ����ֵ��
static void traverseweakvalue (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  // ��������鲿�֣������������а�ɫֵ��ֻ��Ϊ�˼�鲢��ֵ�����ڱ�������
  int hasclears = (h->sizearray > 0);
  // ����Hash����
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    // �������
    checkdeadkey(n);
    // ֵ�Ƿ�Ϊ��
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else {
      lua_assert(!ttisnil(gkey(n)));
      // ���ֵ
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
// ��������ǿֵ��
static int traverseephemeron (global_State *g, Table *h) {
  int marked = 0;  /* true if an object is marked in this traversal */
  int hasclears = 0;  /* true if table has white keys */
  int hasww = 0;  /* true if table has entry "white-key -> white-value" */
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  /* traverse array part */
  // �������鲿��
  for (i = 0; i < h->sizearray; i++) {
    if (valiswhite(&h->array[i])) {
      marked = 1;
      reallymarkobject(g, gcvalue(&h->array[i]));
    }
  }
  /* traverse hash part */
  // ����Hash����
  for (n = gnode(h, 0); n < limit; n++) {
    checkdeadkey(n);
    // valueΪ��
    // ֵΪ��
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    // ��û�б��
    else if (iscleared(g, gkey(n))) {  /* key is not marked (yet)? */
      hasclears = 1;  /* table must be cleared */
      // ֵҲû�б����
      if (valiswhite(gval(n)))  /* value not marked yet? */
        hasww = 1;  /* white-white entry */
    }
    // ֵû�б����
    else if (valiswhite(gval(n))) {  /* value not marked yet? */
      marked = 1;
      reallymarkobject(g, gcvalue(gval(n)));  /* mark it now */
    }
  }
  /* link table into proper list */
  // �ҽ��ں��ʵ�������
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasww)  /* table has white->white entries? */
    linkgclist(h, g->ephemeron);  /* have to propagate again */
  else if (hasclears)  /* table has white keys? */
    linkgclist(h, g->allweak);  /* may have to clean white keys */
  return marked;
}

// ����ǿ��
static void traversestrongtable (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  // �������鲿��
  for (i = 0; i < h->sizearray; i++)  /* traverse array part */
    markvalue(g, &h->array[i]);
  // ����Hash����
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    checkdeadkey(n);
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else {
      lua_assert(!ttisnil(gkey(n)));
      // ��Ǽ�
      markvalue(g, gkey(n));  /* mark key */
      // ���ֵ
      markvalue(g, gval(n));  /* mark value */
    }
  }
}

// ������
static lu_mem traversetable (global_State *g, Table *h) {
  const char *weakkey, *weakvalue;
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  // ���Ԫ��
  markobjectN(g, h->metatable);
  if (mode && ttisstring(mode) &&  /* is there a weak mode? */
      ((weakkey = strchr(svalue(mode), 'k')),
       (weakvalue = strchr(svalue(mode), 'v')),
       (weakkey || weakvalue))) {  /* is really weak? */
      // ��������Ϊ��ɫ
    black2gray(h);  /* keep table gray */
    // ǿ��
    if (!weakkey)  /* strong keys? */
      traverseweakvalue(g, h);
    // ǿֵ
    else if (!weakvalue)  /* strong values? */
      traverseephemeron(g, h);
    // ����������ֵ��
    else  /* all weak */
        // ֱ�ӹҽ���allweak������
      linkgclist(h, g->allweak);  /* nothing to traverse now */
  }
  else  /* not weak */
    // ����ǿ��
    traversestrongtable(g, h);
  return sizeof(Table) + sizeof(TValue) * h->sizearray +
                         sizeof(Node) * cast(size_t, allocsizenode(h));
}


/*
** Traverse a prototype. (While a prototype is being build, its
** arrays can be larger than needed; the extra slots are filled with
** NULL, so the use of 'markobjectN')
*/
// ����ԭ�͡� ���ڹ���ԭ��ʱ������������Ա���Ҫ�Ĵ� ����Ĳ�۳���NULL������ʹ��'markobjectN')
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

// ����C�հ�
static lu_mem traverseCclosure (global_State *g, CClosure *cl) {
  int i;
  // ������е�upvalue
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
// ����Lua�հ�
static lu_mem traverseLclosure (global_State *g, LClosure *cl) {
  int i;
  // ���ԭ��
  markobjectN(g, cl->p);  /* mark its prototype */
  // ���upvalues
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

// �����߳�
static lu_mem traversethread (global_State *g, lua_State *th) {
  StkId o = th->stack;
  if (o == NULL)
    return 1;  /* stack not completely built yet */
  lua_assert(g->gcstate == GCSinsideatomic ||
             th->openupval == NULL || isintwups(th));
  // ��־ջ���������Ԫ��
  for (; o < th->top; o++)  /* mark live elements in the stack */
    markvalue(g, o);
  // ���Ľ׶�
  if (g->gcstate == GCSinsideatomic) {  /* final traversal? */
    // ջ����ʵ��β
    StkId lim = th->stack + th->stacksize;  /* real end of stack */
    // ��ջ���ͽ�β֮���ֵ���
    for (; o < lim; o++)  /* clear not-marked stack slice */
      setnilvalue(o);
    /* 'remarkupvals' may have removed thread from 'twups' list */
    // 'remarkupvals'�����Ѿ��� 'twups' �б���ɾ�����߳�
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
// ����һ����ɫ���壬������ɺ�ɫ�������̣߳����ǻ�ɫ�ģ���
static void propagatemark (global_State *g) {
  lu_mem size;
  // �ӻ�ɫ������ȡ��һ������
  GCObject *o = g->gray;
  lua_assert(isgray(o));
  // �����ɺ�ɫ
  gray2black(o);
  switch (o->tt) {
    case LUA_TTABLE: {
      Table *h = gco2t(o);
      // ��gray�������Ƴ�
      g->gray = h->gclist;  /* remove from 'gray' list */
      // ������
      size = traversetable(g, h);
      break;
    }
    case LUA_TLCL: {
      LClosure *cl = gco2lcl(o);
      // ��gray�������Ƴ�
      g->gray = cl->gclist;  /* remove from 'gray' list */
      // ����Lua�հ�
      size = traverseLclosure(g, cl);
      break;
    }
    case LUA_TCCL: {
      CClosure *cl = gco2ccl(o);
      // ��gray�������Ƴ�
      g->gray = cl->gclist;  /* remove from 'gray' list */
      // ����C�հ�
      size = traverseCclosure(g, cl);
      break;
    }
    case LUA_TTHREAD: {
      lua_State *th = gco2th(o);
      // ��gray�������Ƴ�
      g->gray = th->gclist;  /* remove from 'gray' list */
      // ���뵽grayagain������
      linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
      // ��ɻ�ɫ
      black2gray(o);
      // �����߳�
      size = traversethread(g, th);
      break;
    }
    case LUA_TPROTO: {
      Proto *p = gco2p(o);
      // ��gray�������Ƴ�
      g->gray = p->gclist;  /* remove from 'gray' list */
      // ��������ԭ��
      size = traverseproto(g, p);
      break;
    }
    default: lua_assert(0); return;
  }
  g->GCmemtrav += size;
}


// ������еĻ�ɫ
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

// ��tobefnz����ͷȡ������һ��Ԫ�أ��ҽ���allgc�����ͷ��Ȼ�󷵻�
static GCObject *udata2finalize (global_State *g) {
  // ��tobefnz�ĵ�һ��ȡ����
  GCObject *o = g->tobefnz;  /* get first element */
  lua_assert(tofinalize(o));
  g->tobefnz = o->next;  /* remove it from 'tobefnz' list */
  // �ҽӵ�allgc������ȥ
  o->next = g->allgc;  /* return it to 'allgc' list */
  g->allgc = o;
  // ���ÿɻ��ձ��
  resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
  // �������ɨ�׶Σ�������Ϊ��ɫ
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
  // udata2finalize��tobefnz����ͷȡ�µ�һ�������������v
  setgcovalue(L, &v, udata2finalize(g));
  // �õ�TM_GC��Ԫ��
  tm = luaT_gettmbyobj(L, &v, TM_GC);
  // �Ƿ����
  if (tm != NULL && ttisfunction(tm)) {  /* is there a finalizer? */
    int status;
    lu_byte oldah = L->allowhook;
    int running  = g->gcrunning;
    L->allowhook = 0;  /* stop debug hooks during GC metamethod */
    g->gcrunning = 0;  /* avoid GC steps */
    // ��������GC�Ķ�����ջ
    setobj2s(L, L->top, tm);  /* push finalizer... */
    setobj2s(L, L->top + 1, &v);  /* ... and its argument */
    L->top += 2;  /* and (next line) call the finalizer */
    // ����״̬�����ս���״̬
    L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
    // ���ú���
    status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0);
    // �ָ�״̬
    L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
    L->allowhook = oldah;  /* restore hooks */
    g->gcrunning = running;  /* restore state */
    // ����gcʱ����
    if (status != LUA_OK && propagateerrors) {  /* error while running __gc? */
      if (status == LUA_ERRRUN) {  /* is there an error object? */
        // �Ƿ��г�����Ϣ
        const char *msg = (ttisstring(L->top - 1))
                            ? svalue(L->top - 1)
                            : "no message";
        luaO_pushfstring(L, "error in __gc metamethod (%s)", msg);
        status = LUA_ERRGCMM;  /* error in __gc metamethod */
      }
      // �׳�����
      luaD_throw(L, status);  /* re-throw error */
    }
  }
}


/*
** call a few (up to 'g->gcfinnum') finalizers
*/
// �������gcfinnum���ս���
static int runafewfinalizers (lua_State *L) {
  global_State *g = G(L);
  unsigned int i;
  lua_assert(!g->tobefnz || g->gcfinnum > 0);
  // ������g->gcfinnum������g->tobefnz��ȡ����Ԫ��
  for (i = 0; g->tobefnz && i < g->gcfinnum; i++)
    GCTM(L, 1);  /* call one finalizer */
  // ÿ�ζ�������һ��������
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
  // GCestimate�������Ϊlua��ʵ��ռ���ڴ�
  // ��GCѭ��ִ�е�GCScallfin״̬��ǰ,g->GCestimate��gettotalbytes(g)��Ȼ��ȣ������Խ�GCestimate���Ϊ��ǰlua��ʵ��ռ���ڴ�
  // ��MAX_LMEM/estimate��Ϊ��������ڴ����뵱ǰluaʵ��ʹ�����ı�ֵ
  l_mem estimate = g->GCestimate / PAUSEADJ;  /* adjust 'estimate' */
  lua_assert(estimate > 0);
  // ��threshold��Ϊ֮ǰ�������ᵽ�ڴ�ķ�ֵ���÷�ֵ�󲿷�ʱ����ͨ��estimate*gcpause�õ��ġ�gcpauseĬ��ֵΪ100��
  // ��Ȼgcpause���ֵҲ�ǿ���ͨ���ֶ�GC����collectgarbage(��setpause��)���趨�ģ���gcpauseΪ200ʱ����ζ�ţ�
  // threshold = 2GCestimate����debt = -GCestimate(gettotalbytesԼ����GCestimate)��
  // ����GCdebt�����ڴ�������������ڴ�ʱ��-GCestimate����������������֮���ٿ�ʼ�µ�һ��GC������pause����Ϊ����Ъ�ʡ���
  // ����pause�趨Ϊ200ʱ�ͻ����ռ����ȵ����ڴ�ʹ�����ﵽ֮ǰ������ʱ�ſ�ʼ�µ�GCѭ����
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
// �����һ��ɨ��׶Ρ�
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
  // ���ȫ��Ԫ��
  markmt(g);  /* mark global metatables */
  /* remark occasional upvalues of (maybe) dead threads */
  // ���ż�������̵߳�upvalues
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
    // ÿ������GCSWEEPMAX������
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

// singlestepʵ���Ͼ���һ���򵥵�״̬����
static lu_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  switch (g->gcstate) {
      // �Ӹ�����ʼ��ǣ�����ɫ��Ϊ��ɫ�������뵽��ɫ������
      // ��ͣ�׶�
    case GCSpause: {
      g->GCmemtrav = g->strt.size * sizeof(GCObject*);
      // �����ռ�
      restartcollection(g);
      g->gcstate = GCSpropagate;
      return g->GCmemtrav;
    }
    // ��ֳ�׶�
    case GCSpropagate: {
      g->GCmemtrav = 0;
      lua_assert(g->gray);
      propagatemark(g);
      // ֻ��û�л�ɫ��Objects���Ÿı�״̬
       if (g->gray == NULL)  /* no more gray objects? */
        g->gcstate = GCSatomic;  /* finish propagate phase */
      return g->GCmemtrav;  /* memory traversed in this step */
    }
    // ���Ի�ɫ�������һ������ұ�֤��ԭ�Ӳ�����
	// 1���±���(����)������
    // 2����֮ǰ��grayagain(grayagain�ϻ�������Ĵ���), ����������Ŀռ䡣
	// 3����separatetobefnz��������__gc��������Ҫ���յ�(��ɫ)����ŵ�global_State.tobefnz����, �����Ժ�����
	// 4.ʹglobal_State.tobefnz�ϵ����ж���ȫ���ɴ
	// 5.����ǰ��ɫֵ�л�����һ�ֵİ�ɫֵ��
    case GCSatomic: {
      lu_mem work;
      // ��֤��ɫ����Ϊ��
      propagateall(g);  /* make sure gray list is empty */
      // ԭ�ӱ���
      work = atomic(L);  /* work is what was traversed by 'atomic' */
      entersweep(L);
      g->GCestimate = gettotalbytes(g);  /* first estimate */;
      return work;
    }
    // �ݲ�ͬ���͵Ķ��󣬽��зֲ����ա������б�����ͬ���Ͷ���Ĵ洢����
    // GCSswpallgc��ͨ��sweepstep��g->allgc�ϵ������������ͷţ�GCSatomic״̬��ǰ�İ�ɫֵ�Ķ���)��������������±��Ϊ��ǰ��ɫֵ��
    case GCSswpallgc: {  /* sweep "regular" objects */
      return sweepstep(L, g, GCSswpfinobj, &g->finobj);
    }
    // GCSswpfinobj��GCSswptobefnz����״̬Ҳ������sweepstep����������g->finobj��g->tobefnz�������ǲ�������������ģ�
    // ������ǵ����ý����ǽ���Щ������������Ϊ��һ�ֵİ�ɫ��
    // �ö���洢�����Ƿ񵽴���β
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      return sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
    }
    // ����ж϶�����ɫ�Ƿ�Ϊ��
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      return sweepstep(L, g, GCSswpend, NULL);
    }
    // �ͷŶ�����ռ�õĿռ�
    case GCSswpend: {  /* finish sweeps */
      makewhite(g, g->mainthread);  /* sweep main thread */
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      return 0;
    }
    // ��������ɫ��Ϊ��
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
// �õ�GCdebt���ҽ����Kbת����������Ԫ(��ֹ��debt�����)
// ���Ǽ���һ����Ҫ�������ֽ���
static l_mem getdebt (global_State *g) {
  l_mem debt = g->GCdebt;
  int stepmul = g->gcstepmul;
  if (debt <= 0) return 0;  /* minimal debt */
  else {
    debt = (debt / STEPMULADJ) + 1;
    // ���㱾�ε���gc��Ҫ�����Ķ����ֽ���: debt = (debt / STEPMULADJ + 1) * gcstepmul 
    debt = (debt < MAX_LMEM / stepmul) ? debt * stepmul : MAX_LMEM;
    return debt;
  }
}

/*
** performs a basic GC step when collector is running
*/
// ���ռ�������ʱִ�л����� GC ����
 // ��luaC_step��������У�debt������GCdebt���Ǳ����Ա��ʵ�GCdebt��������ʼ�Ϊgcstepmul��
void luaC_step (lua_State *L) {
  global_State *g = G(L);
 
  // ���㱾�ε���gc��Ҫ�����Ķ����ֽ���
  l_mem debt = getdebt(g);  /* GC deficit (be paid now) */
  if (!g->gcrunning) {  /* not running? */
    luaE_setdebt(g, -GCSTEPSIZE * 10);  /* avoid being called too often */
    return;
  }
  // ��GCdebt������ʱ��luaC_step��ͨ������GCdebt��ѭ������singlestep
  // �ظ�ֱ����ͣ�����㹻������(����debt)
  // ��GCdebt�Ŵ���debt���ᵼ�¸�ѭ���Ĵ������ӣ��Ӷ��ӳ���һ�����Ĺ�����������stepmul����Ϊ���������ʡ���
  // �����stepmul�趨�ĺܴ��򽫻ὫGCdebt�Ŵ�ܶ౶����ôGC�����˻���֮ǰ��GC�汾stop-the-world 
  // ��Ϊ����ͼ�ھ����ܶ�Ļ����ڴ棬���������������ѭ���У��������singlestep������GC�ķֲ�����
  do {  /* repeat until pause or enough "credit" (negative debt) */
      // ִ��һ��
    lu_mem work = singlestep(L);  /* perform one single step */
    debt -= work;
  } while (debt > -GCSTEPSIZE && g->gcstate != GCSpause);
  if (g->gcstate == GCSpause)
    setpause(g);  /* pause until next cycle */
  else {
    // ��������Ԫת�����ڴ��С
    debt = (debt / g->gcstepmul) * STEPMULADJ;  /* convert 'work units' to Kb */
    // �����µ�debt������
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


