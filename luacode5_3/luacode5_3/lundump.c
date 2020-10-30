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
// 加载指定类型的n个大小的数据
#define LoadVector(S,b,n)	LoadBlock(S,b,(n)*sizeof((b)[0]))
// 加载块，大小为size
static void LoadBlock (LoadState *S, void *b, size_t size) {
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, "truncated");
}


#define LoadVar(S,x)		LoadVector(S,&x,1)

// 加载一个字节
static lu_byte LoadByte (LoadState *S) {
  lu_byte x;
  LoadVar(S, x);
  return x;
}

// 加载一个int
static int LoadInt (LoadState *S) {
  int x;
  LoadVar(S, x);
  return x;
}

// 加载一个number
static lua_Number LoadNumber (LoadState *S) {
  lua_Number x;
  LoadVar(S, x);
  return x;
}

// 加载一个lua整形
static lua_Integer LoadInteger (LoadState *S) {
  lua_Integer x;
  LoadVar(S, x);
  return x;
}

// 加载字符串
static TString *LoadString (LoadState *S) {
  // 加载字节
  size_t size = LoadByte(S);
  // 如果大小为0xFF，重新加载
  if (size == 0xFF)
    LoadVar(S, size);
  if (size == 0)
    return NULL;
  // 是否为短字符串
  else if (--size <= LUAI_MAXSHORTLEN) {  /* short string? */
    char buff[LUAI_MAXSHORTLEN];
    LoadVector(S, buff, size);
    return luaS_newlstr(S->L, buff, size);
  }
  // 长字符串
  else {  /* long string */
    TString *ts = luaS_createlngstrobj(S->L, size);
    LoadVector(S, getstr(ts), size);  /* load directly in final place */
    return ts;
  }
}

// 加载代码
static void LoadCode (LoadState *S, Proto *f) {
  // 加载代码的长度
  int n = LoadInt(S);
  // 创建一段内存用来存放代码
  f->code = luaM_newvector(S->L, n, Instruction);
  f->sizecode = n;
  LoadVector(S, f->code, n);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);

// 加载常量
static void LoadConstants (LoadState *S, Proto *f) {
  int i;
  // 加载常量数目
  int n = LoadInt(S);
  f->k = luaM_newvector(S->L, n, TValue);
  f->sizek = n;
  // 将新分配出来的清零
  for (i = 0; i < n; i++)
    setnilvalue(&f->k[i]);

  // 每个常量先加载一个byte的类型，在加载对应的值
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

//加载函数
static void LoadProtos (LoadState *S, Proto *f) {
  int i;
  // 加载内嵌函数的数目
  int n = LoadInt(S);
  f->p = luaM_newvector(S->L, n, Proto *);
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = NULL;
  // 加载内嵌函数
  for (i = 0; i < n; i++) {
    f->p[i] = luaF_newproto(S->L);
    LoadFunction(S, f->p[i], f->source);
  }
}

// 加载upvalues
static void LoadUpvalues (LoadState *S, Proto *f) {
  int i, n;
  // 加载int
  n = LoadInt(S);
  // 创建n个upvalue
  f->upvalues = luaM_newvector(S->L, n, Upvaldesc);
  f->sizeupvalues = n;
  // 将upvalues的名字置空
  for (i = 0; i < n; i++)
    f->upvalues[i].name = NULL;
  // 将upvalues的赋值
  for (i = 0; i < n; i++) {
    f->upvalues[i].instack = LoadByte(S);
    f->upvalues[i].idx = LoadByte(S);
  }
}

// 加载调试信息
static void LoadDebug (LoadState *S, Proto *f) {
  int i, n;
  // 操作码到源代码行信息的映射数目
  n = LoadInt(S);
  f->lineinfo = luaM_newvector(S->L, n, int);
  f->sizelineinfo = n;

  // 加载调试的操作码到源代码的映射
  LoadVector(S, f->lineinfo, n);
  
  // 局部变量的数目
  n = LoadInt(S);
  f->locvars = luaM_newvector(S->L, n, LocVar);
  f->sizelocvars = n;
  // 初始化局部变量的数目
  for (i = 0; i < n; i++)
    f->locvars[i].varname = NULL;

  // 加载局部变量信息
  for (i = 0; i < n; i++) {
    f->locvars[i].varname = LoadString(S);
    f->locvars[i].startpc = LoadInt(S);
    f->locvars[i].endpc = LoadInt(S);
  }
  // 加载upvalues的名字
  n = LoadInt(S);
  for (i = 0; i < n; i++)
    f->upvalues[i].name = LoadString(S);
}

// 加载函数
static void LoadFunction (LoadState *S, Proto *f, TString *psource) {
  // 加载函数的源码
  f->source = LoadString(S);
  // 没有加载到源码就使用传入
  if (f->source == NULL)  /* no source in dump? */
    f->source = psource;  /* reuse parent's source */
  // 
  f->linedefined = LoadInt(S);
  f->lastlinedefined = LoadInt(S);
  // 参数数目
  f->numparams = LoadByte(S);
  // 是否是可变参数
  f->is_vararg = LoadByte(S);
  // 最大的堆栈大小
  f->maxstacksize = LoadByte(S);
  // 加载指令码相关
  LoadCode(S, f);
  // 加载常量
  LoadConstants(S, f);
  // 加载upvalues
  LoadUpvalues(S, f);
  // 加载内嵌函数
  LoadProtos(S, f);
  // 加载调试信息
  LoadDebug(S, f);
}

// 检查编译代码的标志和版本相关的
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
  // luac的版本不匹配
  if (LoadByte(S) != LUAC_VERSION)
    error(S, "version mismatch in");
  // luac的格式不匹配
  if (LoadByte(S) != LUAC_FORMAT)
    error(S, "format mismatch in");
  checkliteral(S, LUAC_DATA, "corrupted");
  // 检查下面的类型的大小
  checksize(S, int);
  checksize(S, size_t);
  checksize(S, Instruction);
  checksize(S, lua_Integer);
  checksize(S, lua_Number);
  // 字节序不匹配
  if (LoadInteger(S) != LUAC_INT)
    error(S, "endianness mismatch in");
  // 浮点格式不匹配
  if (LoadNumber(S) != LUAC_NUM)
    error(S, "float format mismatch in");
}


/*
** load precompiled chunk
*/
// 加载预编译块
LClosure *luaU_undump(lua_State *L, ZIO *Z, const char *name) {
  LoadState S;
  LClosure *cl;
  // 如果名字以@和=开头，跳过
  if (*name == '@' || *name == '=')
    S.name = name + 1;
  // 预编译代码
  else if (*name == LUA_SIGNATURE[0])
    S.name = "binary string";
  else
    S.name = name;
  S.L = L;
  S.Z = Z;
  checkHeader(&S);
  // 创建一个新的Lua闭包
  cl = luaF_newLclosure(L, LoadByte(&S));
  // 将创建的闭包压入栈顶
  setclLvalue(L, L->top, cl);
  // 然后增长堆栈
  luaD_inctop(L);
  cl->p = luaF_newproto(L);
  // 加载函数
  LoadFunction(&S, cl->p, NULL);
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luai_verifycode(L, buff, cl->p);
  return cl;
}

