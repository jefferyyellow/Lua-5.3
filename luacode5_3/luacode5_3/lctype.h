/*
** $Id: lctype.h,v 1.12.1.1 2013/04/12 18:48:47 roberto Exp $
** 'ctype' functions for Lua
** See Copyright Notice in lua.h
*/

#ifndef lctype_h
#define lctype_h

#include "lua.h"


/*
** WARNING: the functions defined here do not necessarily correspond
** to the similar functions in the standard C ctype.h. They are
** optimized for the specific needs of Lua
*/

#if !defined(LUA_USE_CTYPE)

#if 'A' == 65 && '0' == 48
/* ASCII case: can use its own tables; faster and fixed */
#define LUA_USE_CTYPE	0
#else
/* must use standard C ctype */
#define LUA_USE_CTYPE	1
#endif

#endif


#if !LUA_USE_CTYPE	/* { */

#include <limits.h>

#include "llimits.h"

// 字母
#define ALPHABIT	0
// 十进制数字
#define DIGITBIT	1
// 可打印
#define PRINTBIT	2
// 空白字符
#define SPACEBIT	3
// 十六进制数字
#define XDIGITBIT	4


#define MASK(B)		(1 << (B))


/*
** add 1 to char to allow index -1 (EOZ)
*/
#define testprop(c,p)	(luai_ctype_[(c)+1] & (p))

/*
** 'lalpha' (Lua alphabetic) and 'lalnum' (Lua alphanumeric) both include '_'
*/
// 检查所传的字符是否是字母
#define lislalpha(c)	testprop(c, MASK(ALPHABIT))
// 检查所传的字符是否是字母和数字
#define lislalnum(c)	testprop(c, (MASK(ALPHABIT) | MASK(DIGITBIT)))
// 用来检测一个字符是否是十进制数字
#define lisdigit(c)	testprop(c, MASK(DIGITBIT))
// 判断字符是否为空白字符
#define lisspace(c)	testprop(c, MASK(SPACEBIT))
// 检查所传的字符是否是可打印的。
#define lisprint(c)	testprop(c, MASK(PRINTBIT))
// 检查所传的字符是否是十六进制数字
#define lisxdigit(c)	testprop(c, MASK(XDIGITBIT))

/*
** this 'ltolower' only works for alphabetic characters
*/
// 转换成小写字母
#define ltolower(c)	((c) | ('A' ^ 'a'))


/* two more entries for 0 and -1 (EOZ) */
// 多加了0和-1的，所有多了2个，用来测试是数字，字母，空格，等等
LUAI_DDEC const lu_byte luai_ctype_[UCHAR_MAX + 2];


#else			/* }{ */

/*
** use standard C ctypes
*/

#include <ctype.h>

// 检查所传的字符是否是字母
#define lislalpha(c)	(isalpha(c) || (c) == '_')
// 检查所传的字符是否是字母和数字
#define lislalnum(c)	(isalnum(c) || (c) == '_')
// 用来检测一个字符是否是十进制数字
#define lisdigit(c)	(isdigit(c))
// 判断字符是否为空白字符
#define lisspace(c)	(isspace(c))
// 检查所传的字符是否是可打印的。
#define lisprint(c)	(isprint(c))
// 检查所传的字符是否是十六进制数字
#define lisxdigit(c)	(isxdigit(c))

#define ltolower(c)	(tolower(c))

#endif			/* } */

#endif

