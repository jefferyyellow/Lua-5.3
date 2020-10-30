/*
** $Id: lundump.c,v 2.44.1.1 2017/04/19 17:20:42 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#define lundump_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"


#if !defined(luai_verifycode)
#define luai_verifycode(L,b,f)  /* empty */
#endif


typedef struct {
  lua_State *L;
  ZIO *Z;
  const char *name;
} LoadState;


static l_noret error(LoadState *S, const char *why) {
  luaO_pushfstring(S->L, "%s: %s precompiled chunk", S->name, why);
  luaD_throw(S->L, LUA_ERRSYNTAX);
}


/*
** All high-level loads go through LoadVector; you can change it to
** adapt to the endianness of the input
*/
// ����ָ�����͵�n����С������
#define LoadVector(S,b,n)	LoadBlock(S,b,(n)*sizeof((b)[0]))
// ���ؿ飬��СΪsize
static void LoadBlock (LoadState *S, void *b, size_t size) {
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, "truncated");
}


#define LoadVar(S,x)		LoadVector(S,&x,1)

// ����һ���ֽ�
static lu_byte LoadByte (LoadState *S) {
  lu_byte x;
  LoadVar(S, x);
  return x;
}

// ����һ��int
static int LoadInt (LoadState *S) {
  int x;
  LoadVar(S, x);
  return x;
}

// ����һ��number
static lua_Number LoadNumber (LoadState *S) {
  lua_Number x;
  LoadVar(S, x);
  return x;
}

// ����һ��lua����
static lua_Integer LoadInteger (LoadState *S) {
  lua_Integer x;
  LoadVar(S, x);
  return x;
}

// �����ַ���
static TString *LoadString (LoadState *S) {
  // �����ֽ�
  size_t size = LoadByte(S);
  // �����СΪ0xFF�����¼���
  if (size == 0xFF)
    LoadVar(S, size);
  if (size == 0)
    return NULL;
  // �Ƿ�Ϊ���ַ���
  else if (--size <= LUAI_MAXSHORTLEN) {  /* short string? */
    char buff[LUAI_MAXSHORTLEN];
    LoadVector(S, buff, size);
    return luaS_newlstr(S->L, buff, size);
  }
  // ���ַ���
  else {  /* long string */
    TString *ts = luaS_createlngstrobj(S->L, size);
    LoadVector(S, getstr(ts), size);  /* load directly in final place */
    return ts;
  }
}

// ���ش���
static void LoadCode (LoadState *S, Proto *f) {
  // ���ش���ĳ���
  int n = LoadInt(S);
  // ����һ���ڴ�������Ŵ���
  f->code = luaM_newvector(S->L, n, Instruction);
  f->sizecode = n;
  LoadVector(S, f->code, n);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);

// ���س���
static void LoadConstants (LoadState *S, Proto *f) {
  int i;
  // ���س�����Ŀ
  int n = LoadInt(S);
  f->k = luaM_newvector(S->L, n, TValue);
  f->sizek = n;
  // ���·������������
  for (i = 0; i < n; i++)
    setnilvalue(&f->k[i]);

  // ÿ�������ȼ���һ��byte�����ͣ��ڼ��ض�Ӧ��ֵ
  for (i = 0; i < n; i++) {
    TValue *o = &f->k[i];
    int t = LoadByte(S);
    switch (t) {
    case LUA_TNIL:
      setnilvalue(o);
      break;
    case LUA_TBOOLEAN:
      setbvalue(o, LoadByte(S));
      break;
    case LUA_TNUMFLT:
      setfltvalue(o, LoadNumber(S));
      break;
    case LUA_TNUMINT:
      setivalue(o, LoadInteger(S));
      break;
    case LUA_TSHRSTR:
    case LUA_TLNGSTR:
      setsvalue2n(S->L, o, LoadString(S));
      break;
    default:
      lua_assert(0);
    }
  }
}

//���غ���
static void LoadProtos (LoadState *S, Proto *f) {
  int i;
  // ������Ƕ��������Ŀ
  int n = LoadInt(S);
  f->p = luaM_newvector(S->L, n, Proto *);
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = NULL;
  // ������Ƕ����
  for (i = 0; i < n; i++) {
    f->p[i] = luaF_newproto(S->L);
    LoadFunction(S, f->p[i], f->source);
  }
}

// ����upvalues
static void LoadUpvalues (LoadState *S, Proto *f) {
  int i, n;
  // ����int
  n = LoadInt(S);
  // ����n��upvalue
  f->upvalues = luaM_newvector(S->L, n, Upvaldesc);
  f->sizeupvalues = n;
  // ��upvalues�������ÿ�
  for (i = 0; i < n; i++)
    f->upvalues[i].name = NULL;
  // ��upvalues�ĸ�ֵ
  for (i = 0; i < n; i++) {
    f->upvalues[i].instack = LoadByte(S);
    f->upvalues[i].idx = LoadByte(S);
  }
}

// ���ص�����Ϣ
static void LoadDebug (LoadState *S, Proto *f) {
  int i, n;
  // �����뵽Դ��������Ϣ��ӳ����Ŀ
  n = LoadInt(S);
  f->lineinfo = luaM_newvector(S->L, n, int);
  f->sizelineinfo = n;

  // ���ص��ԵĲ����뵽Դ�����ӳ��
  LoadVector(S, f->lineinfo, n);
  
  // �ֲ���������Ŀ
  n = LoadInt(S);
  f->locvars = luaM_newvector(S->L, n, LocVar);
  f->sizelocvars = n;
  // ��ʼ���ֲ���������Ŀ
  for (i = 0; i < n; i++)
    f->locvars[i].varname = NULL;

  // ���ؾֲ�������Ϣ
  for (i = 0; i < n; i++) {
    f->locvars[i].varname = LoadString(S);
    f->locvars[i].startpc = LoadInt(S);
    f->locvars[i].endpc = LoadInt(S);
  }
  // ����upvalues������
  n = LoadInt(S);
  for (i = 0; i < n; i++)
    f->upvalues[i].name = LoadString(S);
}

// ���غ���
static void LoadFunction (LoadState *S, Proto *f, TString *psource) {
  // ���غ�����Դ��
  f->source = LoadString(S);
  // û�м��ص�Դ���ʹ�ô���
  if (f->source == NULL)  /* no source in dump? */
    f->source = psource;  /* reuse parent's source */
  // 
  f->linedefined = LoadInt(S);
  f->lastlinedefined = LoadInt(S);
  // ������Ŀ
  f->numparams = LoadByte(S);
  // �Ƿ��ǿɱ����
  f->is_vararg = LoadByte(S);
  // ���Ķ�ջ��С
  f->maxstacksize = LoadByte(S);
  // ����ָ�������
  LoadCode(S, f);
  // ���س���
  LoadConstants(S, f);
  // ����upvalues
  LoadUpvalues(S, f);
  // ������Ƕ����
  LoadProtos(S, f);
  // ���ص�����Ϣ
  LoadDebug(S, f);
}

// ���������ı�־�Ͱ汾��ص�
static void checkliteral (LoadState *S, const char *s, const char *msg) {
  char buff[sizeof(LUA_SIGNATURE) + sizeof(LUAC_DATA)]; /* larger than both */
  size_t len = strlen(s);
  LoadVector(S, buff, len);
  if (memcmp(s, buff, len) != 0)
    error(S, msg);
}

// 
static void fchecksize (LoadState *S, size_t size, const char *tname) {
  if (LoadByte(S) != size)
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", tname));
}


#define checksize(S,t)	fchecksize(S,sizeof(t),#t)

static void checkHeader (LoadState *S) {
  checkliteral(S, LUA_SIGNATURE + 1, "not a");  /* 1st char already checked */
  // luac�İ汾��ƥ��
  if (LoadByte(S) != LUAC_VERSION)
    error(S, "version mismatch in");
  // luac�ĸ�ʽ��ƥ��
  if (LoadByte(S) != LUAC_FORMAT)
    error(S, "format mismatch in");
  checkliteral(S, LUAC_DATA, "corrupted");
  // �����������͵Ĵ�С
  checksize(S, int);
  checksize(S, size_t);
  checksize(S, Instruction);
  checksize(S, lua_Integer);
  checksize(S, lua_Number);
  // �ֽ���ƥ��
  if (LoadInteger(S) != LUAC_INT)
    error(S, "endianness mismatch in");
  // �����ʽ��ƥ��
  if (LoadNumber(S) != LUAC_NUM)
    error(S, "float format mismatch in");
}


/*
** load precompiled chunk
*/
// ����Ԥ�����
LClosure *luaU_undump(lua_State *L, ZIO *Z, const char *name) {
  LoadState S;
  LClosure *cl;
  // ���������@��=��ͷ������
  if (*name == '@' || *name == '=')
    S.name = name + 1;
  // Ԥ�������
  else if (*name == LUA_SIGNATURE[0])
    S.name = "binary string";
  else
    S.name = name;
  S.L = L;
  S.Z = Z;
  checkHeader(&S);
  // ����һ���µ�Lua�հ�
  cl = luaF_newLclosure(L, LoadByte(&S));
  // �������ıհ�ѹ��ջ��
  setclLvalue(L, L->top, cl);
  // Ȼ��������ջ
  luaD_inctop(L);
  cl->p = luaF_newproto(L);
  // ���غ���
  LoadFunction(&S, cl->p, NULL);
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luai_verifycode(L, buff, cl->p);
  return cl;
}

