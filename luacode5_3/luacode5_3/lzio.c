/*
** $Id: lzio.c,v 1.37.1.1 2017/04/19 17:20:42 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/

#define lzio_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"

// 使用reader函数读取数据来填充Zio结构中的缓冲区数据
int luaZ_fill (ZIO *z) {
  size_t size;
  lua_State *L = z->L;
  const char *buff;
  lua_unlock(L);
  // 读取数据
  buff = z->reader(L, z->data, &size);
  lua_lock(L);
  if (buff == NULL || size == 0)
    return EOZ;
  // 返回第一个字符，并且将p往后移，n也设置成size-1
  z->n = size - 1;  /* discount char being returned */
  z->p = buff;
  return cast_uchar(*(z->p++));
}

// 初始Zio结构
void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader, void *data) {
  z->L = L;
  z->reader = reader;
  z->data = data;
  z->n = 0;
  z->p = NULL;
}


/* --------------------------------------------------------------- read --- */
// 
size_t luaZ_read (ZIO *z, void *b, size_t n) {
  while (n) {
    size_t m;
	// 缓冲区里剩下的字节数为0，就调用luaZ_fill填充缓冲区
    if (z->n == 0) {  /* no bytes in buffer? */
      if (luaZ_fill(z) == EOZ)  /* try to read more */
        return n;  /* no more input; return number of missing bytes */
      else {
		// 需要参看luaZ_fill函数，z->n = size - 1和 return cast_uchar(*(z->p++));
        z->n++;  /* luaZ_fill consumed first byte; put it back */
        z->p--;
      }
    }
	// 拷贝读取到目标缓冲区去
    m = (n <= z->n) ? n : z->n;  /* min. between n and z->n */
    memcpy(b, z->p, m);
	// 各种指针的修正和大小的修正
    z->n -= m;
    z->p += m;
    b = (char *)b + m;
    n -= m;
  }
  return 0;
}

