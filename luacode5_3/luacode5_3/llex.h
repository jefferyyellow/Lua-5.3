/*
** $Id: llex.h,v 1.79.1.1 2017/04/19 17:20:42 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
// 保留字标志的终结符，注意和"ORDER RESERVED"中的保留字字符串顺序保持一致
// TK_NAME：表示变量名的字符串
// TK_DBCOLON：双冒号
// TK_DOTS：三个点(...),表示可变参数
// TK_CONCAT：两个点(..),连接符
// TK_IDIV：整除运算符
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON, TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* number of reserved words */
// 保留字的数目
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))

// 注意是union结构，只能保存整数，浮点数或者字符串中的一种
typedef union {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} SemInfo;  /* semantics information */


typedef struct Token {
  int token;            // TOKEN的枚举值
  SemInfo seminfo;      // 存储含有值的token的值,比如token是TK_STRING，那字符串的内容就保存在这里
} Token;


/* state of the lexer plus state of the parser when shared by all
   functions */
// 词法分析器的状态加上被所有函数共享时的解析器的状态
typedef struct LexState {
  int current;  /* current character (charint) */ // 当前读出来的那个字符的ASCII码
  int linenumber;  /* input line counter */ // 当前处理到哪一行
  int lastline;  /* line of last token 'consumed' */ //最后一个token所在的行
  Token t;  /* current token */ // 通过luaX_next读出来的token
  Token lookahead;  /* look ahead token */ // 提前读取的词法符号
  struct FuncState *fs;  /* current function (parser) */ // 语法分析器使用的重要数据结构
  struct lua_State *L;
  ZIO *z;  /* input stream */ // 输入流，zio实例，会从源码文件中，读取一定数量的字符，以供lexer使用
  Mbuffer* buff;  /* buffer for tokens */ // 当当前的token被识别为TK_FLOAT、TK_INT、TK_NAME或者TK_STRING类型时，
							// 这个buff会临时存放被读出来的字符，等token识别完毕后，再赋值到Token
							// 结构的seminfo变量中
  Table *h;  /* to avoid collection/reuse strings */  // 在编译源文件时，常量会临时存放在这里，提升检索效率，也就是提升编译效率
  struct Dyndata *dyd;  /* dynamic structures used by the parser */ // 语法分析器里要用到的结构
  TString *source;  /* current source name */	// 被解析的源文件名称和路径
  TString *envn;  /* environment variable name */ // 就是"_ENV"的TSting表示
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
// 读取下一个Token
LUAI_FUNC void luaX_next (LexState *ls);
// 提前读取下一个Token
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
