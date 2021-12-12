/*
** $Id: lauxlib.c,v 1.289.1.1 2017/04/19 17:20:42 roberto Exp $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/

#define lauxlib_c
#define LUA_LIB

#include "lprefix.h"


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#include "lua.h"

#include "lauxlib.h"


/*
** {======================================================
** Traceback
** =======================================================
*/

// 第一部分的栈大小
#define LEVELS1	10	/* size of the first part of the stack */
// 第二部分的栈大小
#define LEVELS2	11	/* size of the second part of the stack */



/*
** search for 'objidx' in table at index -1.
** return 1 + string at top if find a good name.
*/
// level：递归查找的层数
// objidx：堆栈索引
// 找到和objidx一样的名字的元素的层次名字，层次之间用“.”连接，最多递归level层
static int findfield (lua_State *L, int objidx, int level) {
	// 如果已经到0层了还没找到，就是没有了
  if (level == 0 || !lua_istable(L, -1))
    return 0;  /* not found */
  // 为啥压入nil呢，可以从findindex中发现，如果key为nil的话，返回第一个索引0
  lua_pushnil(L);  /* start 'next' loop */
  while (lua_next(L, -2)) {  /* for each pair in table */
	// 如果键值不是string类型的，跳过
    if (lua_type(L, -2) == LUA_TSTRING) {  /* ignore non-string keys */
		// 栈顶的元素是否和objidx的元素相等
      if (lua_rawequal(L, objidx, -1)) {  /* found object? */
		  // 值出栈
        lua_pop(L, 1);  /* remove value (but keep name) */
        return 1;
      }
	  // 递归往下找
      else if (findfield(L, objidx, level - 1)) {  /* try recursively */
		// 把table删除
        lua_remove(L, -2);  /* remove table (but keep name) */
		// 压入"."
        lua_pushliteral(L, ".");
		// 在两个名字之间插入"."
        lua_insert(L, -2);  /* place '.' between the two names */
		// 将名字连接起来形成a.b的方式
        lua_concat(L, 3);
        return 1;
      }
    }
	// 把值出栈
    lua_pop(L, 1);  /* remove value */
  }
  return 0;  /* not found */
}


/*
** Search for a name for a function in all loaded modules
*/
// 在所有加载模块中查找函数的名字
static int pushglobalfuncname (lua_State *L, lua_Debug *ar) {
	// 记录栈顶
  int top = lua_gettop(L);
  lua_getinfo(L, "f", ar);  /* push function */
  // 得到记录已经加载模块的表
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  // 得到函数的名字
  if (findfield(L, top + 1, 2)) {
    const char *name = lua_tostring(L, -1);
	// 如果名字以_G.开头
    if (strncmp(name, "_G.", 3) == 0) {  /* name start with '_G.'? */
		// 过滤掉前面的_G.前缀，并且压栈
      lua_pushstring(L, name + 3);  /* push name without prefix */
	  // 将原来的名字删除
      lua_remove(L, -2);  /* remove original name */
    }
	// 将名字拷贝到合适的地方
    lua_copy(L, -1, top + 1);  /* move name to proper place */
	// 调整堆栈
    lua_pop(L, 2);  /* remove pushed values */
    return 1;
  }
  else {
	 // 调整堆栈
    lua_settop(L, top);  /* remove function and global table */
    return 0;
  }
}

// 查找函数名
static void pushfuncname (lua_State *L, lua_Debug *ar) {
	// 从全局中查找
  if (pushglobalfuncname(L, ar)) {  /* try first a global name */
	  // 以“function 函数名字”的格式压栈
    lua_pushfstring(L, "function '%s'", lua_tostring(L, -1));
    lua_remove(L, -2);  /* remove name */
  }
  // 代码中的函数名
  else if (*ar->namewhat != '\0')  /* is there a name from code? */
    lua_pushfstring(L, "%s '%s'", ar->namewhat, ar->name);  /* use it */
  // main函数
  else if (*ar->what == 'm')  /* main? */
      lua_pushliteral(L, "main chunk");
  // 
  else if (*ar->what != 'C')  /* for Lua functions, use <file:line> */
    lua_pushfstring(L, "function <%s:%d>", ar->short_src, ar->linedefined);
  else  /* nothing left... */
    lua_pushliteral(L, "?");
}

// 最大的堆栈层数
static int lastlevel (lua_State *L) {
  lua_Debug ar;
  int li = 1, le = 1;
  /* find an upper bound */
  // 成倍增加，2的指数次，找到第一次出错的level：le，和最后一次正确的level：li
  while (lua_getstack(L, le, &ar)) { li = le; le *= 2; }
  // 在用2分法查找
  /* do a binary search */
  while (li < le) {
    int m = (li + le)/2;
    if (lua_getstack(L, m, &ar)) li = m + 1;
    else le = m;
  }
  return le - 1;
}

// 堆栈回溯
LUALIB_API void luaL_traceback (lua_State *L, lua_State *L1,
                                const char *msg, int level) {
  lua_Debug ar;
  // 栈元素数目
  int top = lua_gettop(L);
  // 最大堆栈层数
  int last = lastlevel(L1);
  // 计算前面的打印部分层数
  int n1 = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  if (msg)
    lua_pushfstring(L, "%s\n", msg);
  luaL_checkstack(L, 10, NULL);
  lua_pushliteral(L, "stack traceback:");
  // 得到对应的栈信息
  while (lua_getstack(L1, level++, &ar)) {
	  // 前面的已经打印完成了
    if (n1-- == 0) {  /* too many levels? */
      lua_pushliteral(L, "\n\t...");  /* add a '...' */
	  // 计算后面的打印层数
      level = last - LEVELS2 + 1;  /* and skip to last ones */
    }
    else {
		// 得到堆栈信息
      lua_getinfo(L1, "Slnt", &ar);
	  // 描述
      lua_pushfstring(L, "\n\t%s:", ar.short_src);
	  // 行号信息
      if (ar.currentline > 0)
        lua_pushfstring(L, "%d:", ar.currentline);
	  // 
      lua_pushliteral(L, " in ");
	  // 文件名
      pushfuncname(L, &ar);
	  // 是否是尾调用
      if (ar.istailcall)
        lua_pushliteral(L, "\n\t(...tail calls...)");
      lua_concat(L, lua_gettop(L) - top);
    }
  }
  lua_concat(L, lua_gettop(L) - top);
}

/* }====================================================== */


/*
** {======================================================
** Error-report functions
** =======================================================
*/

LUALIB_API int luaL_argerror (lua_State *L, int arg, const char *extramsg) {
  lua_Debug ar;
  if (!lua_getstack(L, 0, &ar))  /* no stack frame? */
    return luaL_error(L, "bad argument #%d (%s)", arg, extramsg);
  lua_getinfo(L, "n", &ar);
  if (strcmp(ar.namewhat, "method") == 0) {
    arg--;  /* do not count 'self' */
    if (arg == 0)  /* error is in the self argument itself? */
      return luaL_error(L, "calling '%s' on bad self (%s)",
                           ar.name, extramsg);
  }
  if (ar.name == NULL)
    ar.name = (pushglobalfuncname(L, &ar)) ? lua_tostring(L, -1) : "?";
  return luaL_error(L, "bad argument #%d to '%s' (%s)",
                        arg, ar.name, extramsg);
}

// 类型错误
static int typeerror (lua_State *L, int arg, const char *tname) {
  const char *msg;
  const char *typearg;  /* name for the type of the actual argument */
  if (luaL_getmetafield(L, arg, "__name") == LUA_TSTRING)
    typearg = lua_tostring(L, -1);  /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = "light userdata";  /* special name for messages */
  else
    typearg = luaL_typename(L, arg);  /* standard name */
  msg = lua_pushfstring(L, "%s expected, got %s", tname, typearg);
  return luaL_argerror(L, arg, msg);
}


static void tag_error (lua_State *L, int arg, int tag) {
  typeerror(L, arg, lua_typename(L, tag));
}


/*
** The use of 'lua_pushfstring' ensures this function does not
** need reserved stack space when called.
*/
LUALIB_API void luaL_where (lua_State *L, int level) {
  lua_Debug ar;
  // 得到指定level的堆栈
  if (lua_getstack(L, level, &ar)) {  /* check function at level */
	  // 得到堆栈的信息
    lua_getinfo(L, "Sl", &ar);  /* get info about it */
	// 如果有对应的行信息，压入堆栈
    if (ar.currentline > 0) {  /* is there info? */
      lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
      return;
    }
  }
  lua_pushfstring(L, "");  /* else, no information available... */
}


/*
** Again, the use of 'lua_pushvfstring' ensures this function does
** not need reserved stack space when called. (At worst, it generates
** an error with "stack overflow" instead of the given message.)
*/
// 同样，当调用‘lua_pushvfstring’时，它的用法保证了该函数不需要预留栈空间
// （最坏的情况下，它通过产生一个“栈溢出”的错误来替代给出错误信息）
LUALIB_API int luaL_error (lua_State *L, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  luaL_where(L, 1);
  // 格式化字符串并压栈
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_concat(L, 2);
  return lua_error(L);
}

// 这个函数用于生成标准库中和文件相关的函数的返回值。 （指 (io.open， os.rename， file:seek，等。)。
LUALIB_API int luaL_fileresult (lua_State *L, int stat, const char *fname) {
  int en = errno;  /* calls to Lua API may change this value */
  // 如果状态不为0,将1压栈
  if (stat) {
    lua_pushboolean(L, 1);
    return 1;
  }
  else {
	  // 如果状态为0,将nil压栈
    lua_pushnil(L);
	// 出错原因压栈
    if (fname)
      lua_pushfstring(L, "%s: %s", fname, strerror(en));
    else
      lua_pushstring(L, strerror(en));
	// 错误码压栈
    lua_pushinteger(L, en);
    return 3;
  }
}


#if !defined(l_inspectstat)	/* { */

#if defined(LUA_USE_POSIX)

#include <sys/wait.h>

/*
** use appropriate macros to interpret 'pclose' return status
*/
// 使用合适的宏解释'pclose'返回状态

#define l_inspectstat(stat,what)  \
	// WIFEXITED 取出的字段值非零 -> 正常终止， WEXITSTATUS 取出的字段值就是子进程的退出状态
   if (WIFEXITED(stat)) { stat = WEXITSTATUS(stat); } \
	//WIFSIGNALED 取出的字段值非零-> 异常终止， WTERMSIG 取出的字段值就是信号的编号
   else if (WIFSIGNALED(stat)) { stat = WTERMSIG(stat); what = "signal"; }

#else

#define l_inspectstat(stat,what)  /* no op */

#endif

#endif				/* } */

// 这个函数用于生成标准库中和进程相关函数的返回值。 （指 os.execute 和 io.close）。
LUALIB_API int luaL_execresult (lua_State *L, int stat) {
  const char *what = "exit";  /* type of termination */
  if (stat == -1)  /* error? */
    return luaL_fileresult(L, 0, NULL);
  else {
	  // 解释退出状态
    l_inspectstat(stat, what);  /* interpret result */
	// 正常退出
    if (*what == 'e' && stat == 0)  /* successful termination? */
      lua_pushboolean(L, 1);
	// 异常退出
    else
      lua_pushnil(L);
    lua_pushstring(L, what);
    lua_pushinteger(L, stat);
    return 3;  /* return true/nil,what,code */
  }
}

/* }====================================================== */


/*
** {======================================================
** Userdata's metatable manipulation
** =======================================================
*/
// 创建新元表
LUALIB_API int luaL_newmetatable (lua_State *L, const char *tname) {
	// 是否有同名的元表
  if (luaL_getmetatable(L, tname) != LUA_TNIL)  /* name already in use? */
    return 0;  /* leave previous value on top, but return 0 */
  lua_pop(L, 1);
  // 创建元表
  lua_createtable(L, 0, 2);  /* create metatable */
  lua_pushstring(L, tname);
  // 设置元表的名字
  lua_setfield(L, -2, "__name");  /* metatable.__name = tname */
  lua_pushvalue(L, -1);
  // 将元表在注册表中注册
  lua_setfield(L, LUA_REGISTRYINDEX, tname);  /* registry.name = metatable */
  return 1;
}

// 将注册表中 tname 关联元表 （参见 luaL_newmetatable） 设为栈顶对象的元表。
LUALIB_API void luaL_setmetatable (lua_State *L, const char *tname) {
  // 得到元表
  luaL_getmetatable(L, tname);
  // 设置原来栈顶对象的元表
  lua_setmetatable(L, -2);
}

// 测试ud索引的用户数据的元表是否就是tname的元表
LUALIB_API void *luaL_testudata (lua_State *L, int ud, const char *tname) {
  void *p = lua_touserdata(L, ud);
  if (p != NULL) {  /* value is a userdata? */
	  // 得到userdata的元表
    if (lua_getmetatable(L, ud)) {  /* does it have a metatable? */
		// 得到正确的元表
      luaL_getmetatable(L, tname);  /* get correct metatable */
	  // 是否是同一个
      if (!lua_rawequal(L, -1, -2))  /* not the same? */
        p = NULL;  /* value is a userdata with wrong metatable */
      lua_pop(L, 2);  /* remove both metatables */
      return p;
    }
  }
  return NULL;  /* value is not a userdata with a metatable */
}

// 检查函数的第 arg 个参数是否是一个类型为 tname 的用户数据 （参见 luaL_newmetatable )。 
// 它会返回该用户数据的地址 （参见 lua_touserdata）。
LUALIB_API void *luaL_checkudata (lua_State *L, int ud, const char *tname) {
  void *p = luaL_testudata(L, ud, tname);
  if (p == NULL) typeerror(L, ud, tname);
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Argument check functions
** =======================================================
*/
// 检查函数的第 arg 个参数是否是一个 字符串，并在数组 lst （比如是零结尾的字符串数组） 中查找这个字符串。 
// 返回匹配到的字符串在数组中的索引号。 如果参数不是字符串，或是字符串在数组中匹配不到，都将抛出错误。
// 如果 def 不为 NULL， 函数就把 def 当作默认值。 默认值在参数 arg 不存在，或该参数是 nil 时生效。
// 这个函数通常用于将字符串映射为 C 枚举量。 （在 Lua 库中做这个转换可以让其使用字符串，而不是数字来做一些选项。）
LUALIB_API int luaL_checkoption (lua_State *L, int arg, const char *def,
                                 const char *const lst[]) {
  const char *name = (def) ? luaL_optstring(L, arg, def) :
                             luaL_checkstring(L, arg);
  int i;
  for (i=0; lst[i]; i++)
    if (strcmp(lst[i], name) == 0)
      return i;
  return luaL_argerror(L, arg,
                       lua_pushfstring(L, "invalid option '%s'", name));
}


/*
** Ensures the stack has at least 'space' extra slots, raising an error
** if it cannot fulfill the request. (The error handling needs a few
** extra slots to format the error message. In case of an error without
** this extra space, Lua will generate the same 'stack overflow' error,
** but without 'msg'.)
*/
// 确定堆栈至少有space的额外slot，如果不能满足要求就触发错误（错误处理需要少许额外的空间去格式化错误信息，
// 一旦一个错误没有这些额外的空间，lua将产生相同于‘stack overflow'的错误，但是没有msg）
LUALIB_API void luaL_checkstack (lua_State *L, int space, const char *msg) {
  if (!lua_checkstack(L, space)) {
    if (msg)
      luaL_error(L, "stack overflow (%s)", msg);
    else
      luaL_error(L, "stack overflow");
  }
}

// 检查函数的第 arg 个参数的类型是否是 t。 参见 lua_type 查阅类型 t 的编码。
LUALIB_API void luaL_checktype (lua_State *L, int arg, int t) {
  if (lua_type(L, arg) != t)
    tag_error(L, arg, t);
}

// 检查函数在 arg 位置是否有任何类型（包括 nil）的参数。
LUALIB_API void luaL_checkany (lua_State *L, int arg) {
  if (lua_type(L, arg) == LUA_TNONE)
    luaL_argerror(L, arg, "value expected");
}

// 检查函数的第 arg 个参数是否是一个 字符串，并返回这个字符串。
LUALIB_API const char *luaL_checklstring (lua_State *L, int arg, size_t *len) {
  const char *s = lua_tolstring(L, arg, len);
  if (!s) tag_error(L, arg, LUA_TSTRING);
  return s;
}

// 如果函数的第 arg 个参数是一个 字符串，返回该字符串。 若该参数不存在或是 nil， 返回 d。 除此之外的情况，抛出错误。
// 若 l 不为 NULL， 将结果的长度填入 *l 。
LUALIB_API const char *luaL_optlstring (lua_State *L, int arg,
                                        const char *def, size_t *len) {
  if (lua_isnoneornil(L, arg)) {
    if (len)
      *len = (def ? strlen(def) : 0);
    return def;
  }
  else return luaL_checklstring(L, arg, len);
}

// 检查函数的第 arg 个参数是否是一个 数字，并返回这个数字。
LUALIB_API lua_Number luaL_checknumber (lua_State *L, int arg) {
  int isnum;
   // 转换成数字
  lua_Number d = lua_tonumberx(L, arg, &isnum);
  if (!isnum)
    tag_error(L, arg, LUA_TNUMBER);
  return d;
}

// 如果堆栈上的第arg个参数是一个数字，返回该数字。 
// 若该参数不存在或是 nil， 返回 d。 除此之外的情况，抛出错误。
LUALIB_API lua_Number luaL_optnumber (lua_State *L, int arg, lua_Number def) {
  return luaL_opt(L, luaL_checknumber, arg, def);
}

// 整形出错的提示
static void interror (lua_State *L, int arg) {
	// 是数字
  if (lua_isnumber(L, arg))
    luaL_argerror(L, arg, "number has no integer representation");
  else
    tag_error(L, arg, LUA_TNUMBER);
}

// 检查堆栈上的第arg个参数是否是一个整数（或是可以被转换为一个整数） 并以 lua_Integer 类型返回这个整数值。
LUALIB_API lua_Integer luaL_checkinteger (lua_State *L, int arg) {
  int isnum;
  // 转换成整数
  lua_Integer d = lua_tointegerx(L, arg, &isnum);
  if (!isnum) {
    interror(L, arg);
  }
  return d;
}

// 如果堆栈上的第arg个参数是一个整数（或可以转换为一个整数）， 返回该整数。 
// 若该参数不存在或是 nil， 返回 d。 除此之外的情况，抛出错误。
LUALIB_API lua_Integer luaL_optinteger (lua_State *L, int arg,
                                                      lua_Integer def) {
  return luaL_opt(L, luaL_checkinteger, arg, def);
}

/* }====================================================== */


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

/* userdata to box arbitrary data */
// 封装任意数据的userdata
typedef struct UBox {
  void *box;		// 数据
  size_t bsize;		// 数据大小
} UBox;

// 重新定义box
static void *resizebox (lua_State *L, int idx, size_t newsize) {
  void *ud;
  lua_Alloc allocf = lua_getallocf(L, &ud);
  UBox *box = (UBox *)lua_touserdata(L, idx);
  void *temp = allocf(ud, box->box, box->bsize, newsize);
  // 分配函数出错
  if (temp == NULL && newsize > 0) {  /* allocation error? */
    resizebox(L, idx, 0);  /* free buffer */
    luaL_error(L, "not enough memory for buffer allocation");
  }
  box->box = temp;
  box->bsize = newsize;
  return temp;
}

// UBox的gc函数
static int boxgc (lua_State *L) {
  resizebox(L, 1, 0);
  return 0;
}

// 创建UBox
static void *newbox (lua_State *L, size_t newsize) {
	// 创建UBox
  UBox *box = (UBox *)lua_newuserdata(L, sizeof(UBox));
  box->box = NULL;
  box->bsize = 0;
  // 创建元表
  if (luaL_newmetatable(L, "LUABOX")) {  /* creating metatable? */
    lua_pushcfunction(L, boxgc);
	// metatable.__gc = boxgc
    lua_setfield(L, -2, "__gc");  /* metatable.__gc = boxgc */
  }
  lua_setmetatable(L, -2);
  return resizebox(L, -1, newsize);
}


/*
** check whether buffer is using a userdata on the stack as a temporary
** buffer
*/
// 检查缓冲区是否使用一个栈上的userdata作为临时缓冲区
// 检查是否使用原始的缓冲区
#define buffonstack(B)	((B)->b != (B)->initb)


/*
** returns a pointer to a free area with at least 'sz' bytes
*/
// 返回一段大小为 sz 的空间地址。 你可以将字符串复制其中以加到缓存 B 内 （参见 luaL_Buffer）。
// 将字符串复制其中后，你必须调用 luaL_addsize 传入字符串的大小，才会真正把它加入缓存。
LUALIB_API char *luaL_prepbuffsize (luaL_Buffer *B, size_t sz) {
  lua_State *L = B->L;
  if (B->size - B->n < sz) {  /* not enough space? */
	// 计算缓冲区的大小
    char *newbuff;
	// 双倍扩充尺寸是否足够
    size_t newsize = B->size * 2;  /* double buffer size */
	// 没有足够尺寸就直接将尺寸调整成sz + B->n（已经使用）
    if (newsize - B->n < sz)  /* not big enough? */
      newsize = B->n + sz;
	// 如果数值异常就报错
    if (newsize < B->n || newsize - B->n < sz)
      luaL_error(L, "buffer too large");
    /* create larger buffer */
	// 是不是已经不是原始的缓冲区了
    if (buffonstack(B))
      newbuff = (char *)resizebox(L, -1, newsize);
    else {  /* no buffer yet */
      newbuff = (char *)newbox(L, newsize);
	  // 将原始数据拷贝进去
      memcpy(newbuff, B->b, B->n * sizeof(char));  /* copy original content */
    }
	// 设置新的缓冲区地址和大小
    B->b = newbuff;
    B->size = newsize;
  }
  return &B->b[B->n];
}

// 将字符串加入luaL_Buffer中
LUALIB_API void luaL_addlstring (luaL_Buffer *B, const char *s, size_t l) {
  if (l > 0) {  /* avoid 'memcpy' when 's' can be NULL */
	  // 准备好缓存
    char *b = luaL_prepbuffsize(B, l);
	// 拷贝进去
    memcpy(b, s, l * sizeof(char));
    luaL_addsize(B, l);
  }
}

// 将字符串加入luaL_Buffer中
LUALIB_API void luaL_addstring (luaL_Buffer *B, const char *s) {
  luaL_addlstring(B, s, strlen(s));
}

// 
LUALIB_API void luaL_pushresult (luaL_Buffer *B) {
  lua_State *L = B->L;
  // 将luaL_Buffer中的字符串压入栈中
  lua_pushlstring(L, B->b, B->n);
  if (buffonstack(B)) {
    resizebox(L, -2, 0);  /* delete old buffer */
    lua_remove(L, -2);  /* remove its header from the stack */
  }
}

// 向缓存B（参见 luaL_Buffer）添加一个已在之前复制到缓冲区的长度为n的字符串（可以看看用法就知道了） 。
LUALIB_API void luaL_pushresultsize (luaL_Buffer *B, size_t sz) {
	// 增加已经复制的长度
  luaL_addsize(B, sz);
  // 将luaL_Buffer中的字符串压入栈中
  luaL_pushresult(B);
}

// 将栈顶的值加入luaL_Buffer，然后将值弹出
LUALIB_API void luaL_addvalue (luaL_Buffer *B) {
  lua_State *L = B->L;
  size_t l;
  // 取得栈顶元素的字符串
  const char *s = lua_tolstring(L, -1, &l);
  if (buffonstack(B))
    lua_insert(L, -2);  /* put value below buffer */
  // 将字符串加入luaL_Buffer
  luaL_addlstring(B, s, l);
  lua_remove(L, (buffonstack(B)) ? -2 : -1);  /* remove value */
}

// 初始化luaL_Buffer
LUALIB_API void luaL_buffinit (lua_State *L, luaL_Buffer *B) {
  B->L = L;
  B->b = B->initb;
  B->n = 0;
  B->size = LUAL_BUFFERSIZE;
}

// 初始化luaL_Buffer并且准备好sz大小的缓冲区，如果luaL_Buffer自带的缓冲区不够，需要重新分配缓冲区
LUALIB_API char *luaL_buffinitsize (lua_State *L, luaL_Buffer *B, size_t sz) {
  luaL_buffinit(L, B);
  return luaL_prepbuffsize(B, sz);
}

/* }====================================================== */


/*
** {======================================================
** Reference system
** =======================================================
*/

/* index of free-list header */
#define freelist	0

// 针对栈顶的对象，创建并返回一个在索引 t 指向的表中的引用 （最后会弹出栈顶对象）。
// 此引用是一个唯一的整数键。 只要你不向表 t 手工添加整数键， luaL_ref 可以保证它返回的键的唯一性。
// 你可以通过调用 lua_rawgeti(L, t, r) 来找回由 r 引用的对象。 函数 luaL_unref 用来释放一个引用关联的对象
// 如果栈顶的对象是 nil， luaL_ref 将返回常量 LUA_REFNIL。 常量 LUA_NOREF 可以保证和 luaL_ref 能返回的其它引用值不同。
LUALIB_API int luaL_ref (lua_State *L, int t) {
  int ref;
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);  /* remove from stack */
    return LUA_REFNIL;  /* 'nil' has a unique fixed reference */
  }
  // 得到索引t处的表中freelist索引的值
  t = lua_absindex(L, t);
  lua_rawgeti(L, t, freelist);  /* get first free element */
  //  ref = t[freelist]把值取出来
  ref = (int)lua_tointeger(L, -1);  /* ref = t[freelist] */
  // 然后从栈里面移除
  lua_pop(L, 1);  /* remove it from stack */

  if (ref != 0) {  /* any free element? */
    // t[freelist] = t[ref]，相当于将ref从列表中移除了
    lua_rawgeti(L, t, ref);  /* remove it from list */
    lua_rawseti(L, t, freelist);  /* (t[freelist] = t[ref]) */
  }
  // 没有空闲的元素，就增加一个，然后把索引返回去
  else  /* no free elements */
    ref = (int)lua_rawlen(L, t) + 1;  /* get a new reference */
  lua_rawseti(L, t, ref);
  return ref;
}

// 释放索引 t 处表的 ref 引用对象 （参见 luaL_ref ）。 此条目会从表中移除以让其引用的对象可被垃圾收集。 而引用 ref 也被回收再次使用。
// 如果 ref 为 LUA_NOREF 或 LUA_REFNIL， luaL_unref 什么也不做。
LUALIB_API void luaL_unref (lua_State *L, int t, int ref) {
  if (ref >= 0) {
    t = lua_absindex(L, t);
    // 将ref的空闲块链接进去
	// t[ref] = t[freelist]
    lua_rawgeti(L, t, freelist);
    lua_rawseti(L, t, ref);  /* t[ref] = t[freelist] */
	// t[freelist] = ref
    lua_pushinteger(L, ref);
    lua_rawseti(L, t, freelist);  /* t[freelist] = ref */
  }
}

/* }====================================================== */


/*
** {======================================================
** Load functions
** =======================================================
*/
// 文件加载
typedef struct LoadF {
	// 已经预读的字符数目
  int n;  /* number of pre-read characters */
  // 打开的文件描述符
  FILE *f;  /* file being read */
  // 读取的缓冲区
  char buff[BUFSIZ];  /* area for reading file */
} LoadF;

// 如果有预读的，先把预读的返回
// 没有的话，直接从文件中读出缓冲区大小的数据，然后返回
static const char *getF (lua_State *L, void *ud, size_t *size) {
  LoadF *lf = (LoadF *)ud;
  (void)L;  /* not used */
  if (lf->n > 0) {  /* are there pre-read characters to be read? */
    *size = lf->n;  /* return them (chars already in buffer) */
    lf->n = 0;  /* no more pre-read characters */
  }
  else {  /* read a block from file */
    /* 'fread' can return > 0 *and* set the EOF flag. If next call to
       'getF' called 'fread', it might still wait for user input.
       The next check avoids this problem. */
    if (feof(lf->f)) return NULL;
    *size = fread(lf->buff, 1, sizeof(lf->buff), lf->f);  /* read block */
  }
  return lf->buff;
}

// 文件出错了，返回错误码，并将错误提示压入栈中
static int errfile (lua_State *L, const char *what, int fnameindex) {
  const char *serr = strerror(errno);
  const char *filename = lua_tostring(L, fnameindex) + 1;
  lua_pushfstring(L, "cannot %s %s: %s", what, filename, serr);
  lua_remove(L, fnameindex);
  return LUA_ERRFILE;
}

// 跳过UTF-8的BOM标记“EFBBBF”
static int skipBOM (LoadF *lf) {
  const char *p = "\xEF\xBB\xBF";  /* UTF-8 BOM mark */
  int c;
  lf->n = 0;
  do {
    c = getc(lf->f);
	// 如果不是BOM标记了就返回
    if (c == EOF || c != *(const unsigned char *)p++) return c;
    lf->buff[lf->n++] = c;  /* to be read by the parser */
  } while (*p != '\0');
  // 前面是一致的，抛弃掉
  lf->n = 0;  /* prefix matched; discard it */
  return getc(lf->f);  /* return next character */
}


/*
** reads the first character of file 'f' and skips an optional BOM mark
** in its beginning plus its first line if it starts with '#'. Returns
** true if it skipped the first line.  In any case, '*cp' has the
** first "valid" character of the file (after the optional BOM and
** a first-line comment).
*/
// 读取f的第一个字符并且跳过在开头的可选BOM标记，如果第一行是以#号开头，也一并跳过
// 如果跳过第一行就返回true，否则，返回false。在任何情况下，
// *cp的值是该文件的第一个“合法”字符（在可选的BOM标记和第一行注释后面）
static int skipcomment (LoadF *lf, int *cp) {
	// 跳过BOM返回后面的第一个字符
  int c = *cp = skipBOM(lf);
  // 如果是注释行
  if (c == '#') {  /* first line is a comment (Unix exec. file)? */
    do {  /* skip first line */
      c = getc(lf->f);
    } while (c != EOF && c != '\n');
	// 跳过行尾
    *cp = getc(lf->f);  /* skip end-of-line, if present */
    return 1;  /* there was a comment */
  }
  else return 0;  /* no comment */
}

// 把一个文件加载为 Lua 代码块。 这个函数使用 lua_load 加载文件中的数据。 代码块的名字被命名为 filename。 
// 如果 filename 为 NULL， 它从标准输入加载。 如果文件的第一行以 # 打头，则忽略这一行。
// mode 字符串的作用同函数 lua_load。
LUALIB_API int luaL_loadfilex (lua_State *L, const char *filename,
                                             const char *mode) {
  LoadF lf;
  int status, readstatus;
  int c;
  // 文件名在栈上的索引
  int fnameindex = lua_gettop(L) + 1;  /* index of filename on the stack */
  // 如果文件名为空，从stdin读取
  if (filename == NULL) {
    lua_pushliteral(L, "=stdin");
    lf.f = stdin;
  }
  else {
	  // 从文件读取
    lua_pushfstring(L, "@%s", filename);
    lf.f = fopen(filename, "r");
    if (lf.f == NULL) return errfile(L, "open", fnameindex);
  }
  // 跳过注释
  if (skipcomment(&lf, &c))  /* read initial portion */
    lf.buff[lf.n++] = '\n';  /* add line to correct line numbers */
  // 二进制文件,以二进制的方式重新打开
  if (c == LUA_SIGNATURE[0] && filename) {  /* binary file? */
    lf.f = freopen(filename, "rb", lf.f);  /* reopen in binary mode */
    if (lf.f == NULL) return errfile(L, "reopen", fnameindex);
	// 跳过注释
    skipcomment(&lf, &c);  /* re-read initial portion */
  }
  // 如果返回的不是结束符，加入BUFF中
  if (c != EOF)
    lf.buff[lf.n++] = c;  /* 'c' is the first character of the stream */
  // 加载文件
  status = lua_load(L, getF, &lf, lua_tostring(L, -1), mode);
  // 加载过程的错误
  readstatus = ferror(lf.f);
  // 如果是文件的话，需要关闭文件描述符
  if (filename) fclose(lf.f);  /* close file (even in case of errors) */
  if (readstatus) {
    lua_settop(L, fnameindex);  /* ignore results from 'lua_load' */
    return errfile(L, "read", fnameindex);
  }
  lua_remove(L, fnameindex);
  return status;
}


typedef struct LoadS {
  const char *s;
  size_t size;
} LoadS;


static const char *getS (lua_State *L, void *ud, size_t *size) {
  LoadS *ls = (LoadS *)ud;
  (void)L;  /* not used */
  if (ls->size == 0) return NULL;
  // 将值赋给输出值，原来的值就没有了
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}

// 把一段缓存加载为一个 Lua 代码块。 这个函数使用 lua_load 来加载 buff 指向的长度为 sz 的内存区。
// 这个函数和 lua_load 返回值相同。 name 作为代码块的名字，用于调试信息和错误消息。 mode 字符串的作用同函数 lua_load。
LUALIB_API int luaL_loadbufferx (lua_State *L, const char *buff, size_t size,
                                 const char *name, const char *mode) {
  LoadS ls;
  ls.s = buff;
  ls.size = size;
  return lua_load(L, getS, &ls, name, mode);
}

//将一个字符串加载为 Lua 代码块。 这个函数使用 lua_load 加载一个零结尾的字符串 s。
// 此函数的返回值和 lua_load 相同。
LUALIB_API int luaL_loadstring (lua_State *L, const char *s) {
  return luaL_loadbuffer(L, s, strlen(s), s);
}

/* }====================================================== */


// 将索引 obj 处对象的元表中 e 域的值压栈。 如果该对象没有元表，或是该元表没有相关域， 此函数什么也不会压栈并返回 LUA_TNIL
LUALIB_API int luaL_getmetafield (lua_State *L, int obj, const char *event) {
	// 得到obj指定的表
  if (!lua_getmetatable(L, obj))  /* no metatable? */
    return LUA_TNIL;
  else {
    int tt;
    lua_pushstring(L, event);
	// 得到对应的值
    tt = lua_rawget(L, -2);
    if (tt == LUA_TNIL)  /* is metafield nil? */
      lua_pop(L, 2);  /* remove metatable and metafield */
    else
      lua_remove(L, -2);  /* remove only metatable */
    return tt;  /* return metafield type */
  }
}

// 调用一个元方法。
// 如果在索引 obj 处的对象有元表， 且元表有域 e 。 这个函数会以该对象为参数调用这个域。 这种情况下，函数返回真并将调用返回值压栈。 
// 如果那个位置没有元表，或没有对应的元方法， 此函数返回假（并不会将任何东西压栈）。
LUALIB_API int luaL_callmeta (lua_State *L, int obj, const char *event) {
  obj = lua_absindex(L, obj);
  // 得到obj处的对象元表中event的字段
  if (luaL_getmetafield(L, obj, event) == LUA_TNIL)  /* no metafield? */
    return 0;
  lua_pushvalue(L, obj);
  // 调用
  lua_call(L, 1, 1);
  return 1;
}

// 以数字形式返回给定索引处值的“长度”； 它等价于在 Lua 中调用 '#' 的操作
// 如果操作结果不是一个整数，则抛出一个错误。 （这种情况只发生在触发元方法时。）
LUALIB_API lua_Integer luaL_len (lua_State *L, int idx) {
  lua_Integer l;
  int isnum;
  lua_len(L, idx);
  l = lua_tointegerx(L, -1, &isnum);
  if (!isnum)
    luaL_error(L, "object length is not an integer");
  lua_pop(L, 1);  /* remove object */
  return l;
}

// 将给定索引处的 Lua 值转换为一个相应格式的 C 字符串。 结果串不仅会压栈，还会由函数返回。 如果 len 不为 NULL ， 它还把字符串长度设到 *len 中。
// 如果该值有一个带 "__tostring" 域的元表， luaL_tolstring 会以该值为参数去调用对应的元方法， 并将其返回值作为结果。
LUALIB_API const char *luaL_tolstring (lua_State *L, int idx, size_t *len) {
	// 调用元方法__tostring
  if (luaL_callmeta(L, idx, "__tostring")) {  /* metafield? */
	  // 返回值不是字符串
	  if (!lua_isstring(L, -1))
      luaL_error(L, "'__tostring' must return a string");
  }
  else {
	  // 如果没有__tostring的元方法，那就根据类型来处理
    switch (lua_type(L, idx)) {
      case LUA_TNUMBER: {
        if (lua_isinteger(L, idx))
          lua_pushfstring(L, "%I", (LUAI_UACINT)lua_tointeger(L, idx));
        else
          lua_pushfstring(L, "%f", (LUAI_UACNUMBER)lua_tonumber(L, idx));
        break;
      }
      case LUA_TSTRING:
        lua_pushvalue(L, idx);
        break;
      case LUA_TBOOLEAN:
        lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
        break;
      case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
      default: {
        int tt = luaL_getmetafield(L, idx, "__name");  /* try name */
        const char *kind = (tt == LUA_TSTRING) ? lua_tostring(L, -1) :
                                                 luaL_typename(L, idx);
        lua_pushfstring(L, "%s: %p", kind, lua_topointer(L, idx));
        if (tt != LUA_TNIL)
          lua_remove(L, -2);  /* remove '__name' */
        break;
      }
    }
  }
  // 转换成字符串
  return lua_tolstring(L, -1, len);
}


/*
** {======================================================
** Compatibility with 5.1 module functions
** =======================================================
*/
#if defined(LUA_COMPAT_MODULE)
// idx为堆栈上的表，fname指定的字段是否存在
static const char *luaL_findtable (lua_State *L, int idx,
                                   const char *fname, int szhint) {
  const char *e;
  // 将栈上的idx再一次压在栈顶
  if (idx) lua_pushvalue(L, idx);
  do {
    e = strchr(fname, '.');
    if (e == NULL) e = fname + strlen(fname);
	// 压入字符串
    lua_pushlstring(L, fname, e - fname);
	// 如果是nil
    if (lua_rawget(L, -2) == LUA_TNIL) {  /* no such field? */
      lua_pop(L, 1);  /* remove this nil */
	  // 创建一个table
      lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); /* new table for field */
	  // 名字压参
      lua_pushlstring(L, fname, e - fname);
	  // table再一次压参
      lua_pushvalue(L, -2);
	  // 设置新的table到指定field
      lua_settable(L, -4);  /* set new table into field */
    }
    else if (!lua_istable(L, -1)) {  /* field has a non-table value? */
      lua_pop(L, 2);  /* remove table and value */
      return fname;  /* return problematic part of the name */
    }
	// 注意：会把前一个table都删除掉
    lua_remove(L, -2);  /* remove previous table */
    fname = e + 1;
  } while (*e == '.');
  return NULL;
}


/*
** Count number of elements in a luaL_Reg list.
*/
// 计算luaL_Reg列表中元素的数目
static int libsize (const luaL_Reg *l) {
  int size = 0;
  for (; l && l->name; l++) size++;
  return size;
}


/*
** Find or create a module table with a given name. The function
** first looks at the LOADED table and, if that fails, try a
** global variable with that name. In any case, leaves on the stack
** the module table.
*/
// 查找或者创建给定名字的模块表。该函数首先查找LOADED表，如果失败，
// 尝试具有该名字的全局变量，无论如何，将module表留在堆栈上
LUALIB_API void luaL_pushmodule (lua_State *L, const char *modname,
                                 int sizehint) {
	// 查找LUA_LOADED_TABLE表
  luaL_findtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE, 1);
  if (lua_getfield(L, -1, modname) != LUA_TTABLE) {  /* no LOADED[modname]? */
    lua_pop(L, 1);  /* remove previous result */
    /* try global variable (and create one if it does not exist) */
	// 获得全局表
    lua_pushglobaltable(L);
	// 查找或者创建modename的table
    if (luaL_findtable(L, 0, modname, sizehint) != NULL)
      luaL_error(L, "name conflict for module '%s'", modname);
	// 
    lua_pushvalue(L, -1);
	// LOADED[modname] = new table
    lua_setfield(L, -3, modname);  /* LOADED[modname] = new table */
  }
  lua_remove(L, -2);  /* remove LOADED table */
}

// 创建一个库的表，
LUALIB_API void luaL_openlib (lua_State *L, const char *libname,
                               const luaL_Reg *l, int nup) {
  luaL_checkversion(L);
  if (libname) {
    luaL_pushmodule(L, libname, libsize(l));  /* get/create library table */
    lua_insert(L, -(nup + 1));  /* move library table to below upvalues */
  }
  // 设置函数的upvalues
  if (l)
    luaL_setfuncs(L, l, nup);
  else
	  // 删除upvalues
    lua_pop(L, nup);  /* remove upvalues */
}

#endif
/* }====================================================== */
/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
// 把数组 l 中的所有函数 （参见 luaL_Reg） 注册到栈顶的表中（该表在可选的上值之下，见下面的解说）。
// 若 nup 不为零， 所有的函数都共享 nup 个上值。 这些值必须在调用之前，压在表之上。 这些值在注册完毕后都会从栈弹出。
LUALIB_API void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup) {
  luaL_checkstack(L, nup, "too many upvalues");
  for (; l->name != NULL; l++) {  /* fill the table with given functions */
    int i;
	// 压栈nup个元素
    for (i = 0; i < nup; i++)  /* copy upvalues to the top */
      lua_pushvalue(L, -nup);
	// 闭包对应着upvalues
    lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
    lua_setfield(L, -(nup + 2), l->name);
  }
  // 将原始的nup个upvalues弹出栈
  lua_pop(L, nup);  /* remove upvalues */
}


/*
** ensure that stack[idx][fname] has a table and push that table
** into the stack
*/
// 确保 t[fname] 是一张表，并将这张表压栈。 这里的 t 指索引 idx 处的值。 如果它原来就是一张表，返回真；
// 否则为它创建一张新表，返回假。
LUALIB_API int luaL_getsubtable (lua_State *L, int idx, const char *fname) {
	// 
  if (lua_getfield(L, idx, fname) == LUA_TTABLE)
    return 1;  /* table already there */
  else {
	  // 将原来的结果出栈
    lua_pop(L, 1);  /* remove previous result */
    idx = lua_absindex(L, idx);
	// 创建一个新的table
    lua_newtable(L);
    lua_pushvalue(L, -1);  /* copy to be left at top */
	// 设置table的名字
    lua_setfield(L, idx, fname);  /* assign new table to field */
    return 0;  /* false, because did not find table there */
  }
}


/*
** Stripped-down 'require': After checking "loaded" table, calls 'openf'
** to open a module, registers the result in 'package.loaded' table and,
** if 'glb' is true, also registers the result in the global table.
** Leaves resulting module on the top.
*/
// 如果 modname 不在 package.loaded 中， 则调用函数 openf ，并传入字符串 modname。 将其返回值置入 package.loaded[modname]。 这个行为好似该函数通过 require 调用过一样。
// 如果 glb 为真， 同时也讲模块设到全局变量 modname 里。
LUALIB_API void luaL_requiref (lua_State *L, const char *modname,
                               lua_CFunction openf, int glb) {
	// 得到全局注册表中的加载表
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  // 通过模块名去索引
  lua_getfield(L, -1, modname);  /* LOADED[modname] */
  // 模块没有加载
  if (!lua_toboolean(L, -1)) {  /* package not already loaded? */
	  // 把是否加载的结果弹出
    lua_pop(L, 1);  /* remove field */
	// 压栈openf
    lua_pushcfunction(L, openf);
	// 压栈模块名字
    lua_pushstring(L, modname);  /* argument to open function */
	// 调用openf
    lua_call(L, 1, 1);  /* call 'openf' to open module */
	// 拷贝一份结果
    lua_pushvalue(L, -1);  /* make copy of module (call result) */
	// LOADED[modname] = module
    lua_setfield(L, -3, modname);  /* LOADED[modname] = module */
  }
  lua_remove(L, -2);  /* remove LOADED table */
  if (glb) {
    lua_pushvalue(L, -1);  /* copy of module */
    lua_setglobal(L, modname);  /* _G[modname] = module */
  }
}

// 将字符串 s 生成一个副本， 并将其中的所有字符串 p 都替换为字符串 r 。 将结果串压栈并返回它。
LUALIB_API const char *luaL_gsub (lua_State *L, const char *s, const char *p,
                                                               const char *r) {
  const char *wild;
  // p字符串的长度
  size_t l = strlen(p);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  // 搜索字符串中p字符出现的地方
  while ((wild = strstr(s, p)) != NULL) {
	  // 将出现之前的那部分加入进去
    luaL_addlstring(&b, s, wild - s);  /* push prefix */
	// 将替换字符串加入进去
    luaL_addstring(&b, r);  /* push replacement in place of pattern */
	// 进行下一轮的搜索
    s = wild + l;  /* continue after 'p' */
  }
  // 将最后的部分加入进去
  luaL_addstring(&b, s);  /* push last suffix */
  // 将结果压入堆栈
  luaL_pushresult(&b);
  return lua_tostring(L, -1);
}

// 内存分配函数
static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud; (void)osize;  /* not used */
  // 大小为0表示，释放内存
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  else
	// 重新分配
    return realloc(ptr, nsize);
}

// 出错的默认打印函数
static int panic (lua_State *L) {
  lua_writestringerror("PANIC: unprotected error in call to Lua API (%s)\n",
                        lua_tostring(L, -1));
  return 0;  /* return to Lua to abort */
}

// 创建一个新的 Lua 状态机。 它以一个基于标准 C 的 realloc 函数实现的内存分配器 调用 lua_newstate 。 
// 并把可打印一些出错信息到标准错误输出的 panic 函数（参见 §4.6） 设置好，用于处理致命错误。
LUALIB_API lua_State *luaL_newstate (void) {
  lua_State *L = lua_newstate(l_alloc, NULL);
  if (L) lua_atpanic(L, &panic);
  return L;
}

// 检查lua版本号
LUALIB_API void luaL_checkversion_ (lua_State *L, lua_Number ver, size_t sz) {
  const lua_Number *v = lua_version(L);
  // 大小对不对
  if (sz != LUAL_NUMSIZES)  /* check numeric types */
    luaL_error(L, "core and library have incompatible numeric types");
  // 版本号的地址不同，多个lua虚拟机
  if (v != lua_version(NULL))
    luaL_error(L, "multiple Lua VMs detected");
  // 版本号不同。。。
  else if (*v != ver)
    luaL_error(L, "version mismatch: app. needs %f, Lua core provides %f",
                  (LUAI_UACNUMBER)ver, (LUAI_UACNUMBER)*v);
}

