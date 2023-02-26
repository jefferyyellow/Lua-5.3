/*
** $Id: lzio.h,v 1.31.1.1 2017/04/19 17:20:42 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/


#ifndef lzio_h
#define lzio_h

#include "lua.h"

#include "lmem.h"


#define EOZ	(-1)			/* end of stream */

typedef struct Zio ZIO;
// 取得当前的字符，如果没取完，直接移动到下一个指针，如果取完了， 使用reader函数读取数据来填充Zio结构中的缓冲区数据
#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : luaZ_fill(z))


typedef struct Mbuffer {
  char *buffer;
  size_t n;
  size_t buffsize;
} Mbuffer;

#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

#define luaZ_buffer(buff)	((buff)->buffer)
#define luaZ_sizebuffer(buff)	((buff)->buffsize)
#define luaZ_bufflen(buff)	((buff)->n)

// 从buff的末尾删除i个字符
#define luaZ_buffremove(buff,i)	((buff)->n -= (i))
#define luaZ_resetbuffer(buff) ((buff)->n = 0)


#define luaZ_resizebuffer(L, buff, size) \
	((buff)->buffer = luaM_reallocvchar(L, (buff)->buffer, \
				(buff)->buffsize, size), \
	(buff)->buffsize = size)

#define luaZ_freebuffer(L, buff)	luaZ_resizebuffer(L, buff, 0)


LUAI_FUNC void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader,
                                        void *data);
LUAI_FUNC size_t luaZ_read (ZIO* z, void *b, size_t n);	/* read next n bytes */



/* --------- Private Part ------------------ */
// 缓存管理结构
struct Zio {
  size_t n;			/* bytes still unread */ // 缓冲区还有多少个字节没有读取
  const char *p;		/* current position in buffer */ // 缓冲区可以读取的字节开始处
  lua_Reader reader;		/* reader function */		// 读取函数
  void *data;			/* additional data */	// 附加数据，如果reader的getF的时候，data就是LoadF结构的指针，还有可能是其他的，和reader对应就行
  lua_State *L;			/* Lua state (for reader) */ // 读取函数的Lua state
};


LUAI_FUNC int luaZ_fill (ZIO *z);

#endif
