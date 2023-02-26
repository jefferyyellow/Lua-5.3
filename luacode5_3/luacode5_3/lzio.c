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

// ʹ��reader������ȡ���������Zio�ṹ�еĻ���������
int luaZ_fill (ZIO *z) {
  size_t size;
  lua_State *L = z->L;
  const char *buff;
  lua_unlock(L);
  // ��ȡ����
  buff = z->reader(L, z->data, &size);
  lua_lock(L);
  if (buff == NULL || size == 0)
    return EOZ;
  // ���ص�һ���ַ������ҽ�p�����ƣ�nҲ���ó�size-1
  z->n = size - 1;  /* discount char being returned */
  z->p = buff;
  return cast_uchar(*(z->p++));
}

// ��ʼZio�ṹ
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
	// ��������ʣ�µ��ֽ���Ϊ0���͵���luaZ_fill��仺����
    if (z->n == 0) {  /* no bytes in buffer? */
      if (luaZ_fill(z) == EOZ)  /* try to read more */
        return n;  /* no more input; return number of missing bytes */
      else {
		// ��Ҫ�ο�luaZ_fill������z->n = size - 1�� return cast_uchar(*(z->p++));
        z->n++;  /* luaZ_fill consumed first byte; put it back */
        z->p--;
      }
    }
	// ������ȡ��Ŀ�껺����ȥ
    m = (n <= z->n) ? n : z->n;  /* min. between n and z->n */
    memcpy(b, z->p, m);
	// ����ָ��������ʹ�С������
    z->n -= m;
    z->p += m;
    b = (char *)b + m;
    n -= m;
  }
  return 0;
}

