/*
** $Id: llex.c,v 2.96.1.1 2017/04/19 17:20:42 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#define llex_c
#define LUA_CORE

#include "lprefix.h"


#include <locale.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"


// 取得下一个字符并赋值给当前处理的字符
#define next(ls) (ls->current = zgetc(ls->z))


// 是否为换行符
#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED */
// 保留字
static const char *const luaX_tokens [] = {
    "and", "break", "do", "else", "elseif",
    "end", "false", "for", "function", "goto", "if",
    "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
    "//", "..", "...", "==", ">=", "<=", "~=",
    "<<", ">>", "::", "<eof>",
    "<number>", "<integer>", "<name>", "<string>"
};

// 保存当前的字符到LexState的buff中，并取后面一个字符
#define save_and_next(ls) (save(ls, ls->current), next(ls))


static l_noret lexerror (LexState *ls, const char *msg, int token);

// 保存当前的字符到LexState的buff中
static void save (LexState *ls, int c) {
  Mbuffer *b = ls->buff;
  // 如果有溢出风险
  if (luaZ_bufflen(b) + 1 > luaZ_sizebuffer(b)) {
    size_t newsize;
    if (luaZ_sizebuffer(b) >= MAX_SIZE/2)
      lexerror(ls, "lexical element too long", 0);
    // 扩展成原来的2倍
    newsize = luaZ_sizebuffer(b) * 2;
    // 重新分配内存
    luaZ_resizebuffer(ls->L, b, newsize);
  }
  // 保存字符
  b->buffer[luaZ_bufflen(b)++] = cast(char, c);
}


void luaX_init (lua_State *L) {
  int i;
  // 创建环境变量的名字
  TString *e = luaS_newliteral(L, LUA_ENV);  /* create env name */
  // 绝不收集该名字
  luaC_fix(L, obj2gco(e));  /* never collect this name */
  // 变量保留字
  for (i=0; i<NUM_RESERVED; i++) {
    // 创建保留字字符串
    TString *ts = luaS_new(L, luaX_tokens[i]);
    // 保留字绝不收集
    luaC_fix(L, obj2gco(ts));  /* reserved words are never collected */
    // 设置字符串的额外信息
    ts->extra = cast_byte(i+1);  /* reserved word */
  }
}

// token转换成文本
const char *luaX_token2str (LexState *ls, int token) {
  // 单个字节的符号
  if (token < FIRST_RESERVED) {  /* single-byte symbols? */
    lua_assert(token == cast_uchar(token));
    // 直接转换成字符表示
    return luaO_pushfstring(ls->L, "'%c'", token);
  }
  else {
    // 保留字就用保留字字符串
    const char *s = luaX_tokens[token - FIRST_RESERVED];
    if (token < TK_EOS)  /* fixed format (symbols and reserved words)? */
      return luaO_pushfstring(ls->L, "'%s'", s);
    else  /* names, strings, and numerals */
      return s;
  }
}

// 得到Token的文本表示
static const char *txtToken (LexState *ls, int token) {
  // 变量名，字符串，浮点数和整数，直接用对应的值
  switch (token) {
    case TK_NAME: case TK_STRING:
    case TK_FLT: case TK_INT:
      save(ls, '\0');
      return luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff));
    // 其他的将Token转换成文本
    default:
      return luaX_token2str(ls, token);
  }
}

// 词法错误
static l_noret lexerror (LexState *ls, const char *msg, int token) {
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, "%s near %s", msg, txtToken(ls, token));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}

// 语法错误
l_noret luaX_syntaxerror (LexState *ls, const char *msg) {
  lexerror(ls, msg, ls->t.token);
}


/*
** creates a new string and anchors it in scanner's table so that
** it will not be collected until the end of the compilation
** (by that time it should be anchored somewhere)
*/
// 创建一个新字符串并将其锚定在扫描仪的表中，以便在编译结束之前不会被垃圾收集器回收它（一直到那时它应该锚定在某个地方）
TString *luaX_newstring (LexState *ls, const char *str, size_t l) {
  lua_State *L = ls->L;
  TValue *o;  /* entry for 'str' */
  // 创建一个新字符串
  TString *ts = luaS_newlstr(L, str, l);  /* create new string */
  // 放在lua堆栈上
  setsvalue2s(L, L->top++, ts);  /* temporarily anchor it in stack */
  o = luaH_set(L, ls->h, L->top - 1);
  // 真的是新创建的
  if (ttisnil(o)) {  /* not in use yet? */
    /* boolean value does not need GC barrier;
       table has no metatable, so it does not need to invalidate cache */
    // 防止垃圾收集器收集
    setbvalue(o, 1);  /* t[string] = true */
    luaC_checkGC(L);
  }
  // 重用的
  else {  /* string already present */
    ts = tsvalue(keyfromval(o));  /* re-use value previously stored */
  }
  L->top--;  /* remove string from stack */
  return ts;
}


/*
** increment line number and skips newline sequence (any of
** \n, \r, \n\r, or \r\n)
*/
// 增加行号并跳过换行序列
static void inclinenumber (LexState *ls) {
  // 保存当前处理的字符
  int old = ls->current;
  // 校验当前是换行的字符 
  lua_assert(currIsNewline(ls));
  // 取下一个字符
  next(ls);  /* skip '\n' or '\r' */
  // 如果还是换行符，并且和上次的不一样的换行符，需要再取下一个
  // 目的是跳过"\n\r"或者"\r\n"这种组合的换行符
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip '\n\r' or '\r\n' */
  // 增加行号，如果超过整数的最大值，提示语法错误
  if (++ls->linenumber >= MAX_INT)
    lexerror(ls, "chunk has too many lines", 0);
}

// 初始化词法分析器
void luaX_setinput (lua_State *L, LexState *ls, ZIO *z, TString *source,
                    int firstchar) {
  ls->t.token = 0;
  ls->L = L;
  ls->current = firstchar;
  ls->lookahead.token = TK_EOS;  /* no look-ahead token */
  ls->z = z;
  ls->fs = NULL;
  ls->linenumber = 1;
  ls->lastline = 1;
  ls->source = source;
  ls->envn = luaS_newliteral(L, LUA_ENV);  /* get env name */
  luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);  /* initialize buffer */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/

// 如果是预想的字符c，开始处理下一个，并且返回1，或者维持当前字符，返回0
static int check_next1 (LexState *ls, int c) {
  if (ls->current == c) {
    next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check whether current char is in set 'set' (with two chars) and
** saves it
*/
// 检查是否当前的字符是集合set中的一个（这个集合只有2个字符）
static int check_next2 (LexState *ls, const char *set) {
  lua_assert(set[2] == '\0');
  // 如果当前字符等于集合中的第一个或者第二个字符，就表示符合条件
  if (ls->current == set[0] || ls->current == set[1]) {
    save_and_next(ls);
    return 1;
  }
  else return 0;
}


/* LUA_NUMBER */
/*
** this function is quite liberal in what it accepts, as 'luaO_str2num'
** will reject ill-formed numerals.
*/
// 读取数字
// 该函数接受的内容非常自由，因为“luaO_str2num”将拒绝格式错误的数字。
static int read_numeral (LexState *ls, SemInfo *seminfo) {
  TValue obj;
  // 指数方式
  const char *expo = "Ee";
  int first = ls->current;
  lua_assert(lisdigit(ls->current));
  save_and_next(ls);
  // 是否位0x开头，也就是十六进制，指数方式为p
  if (first == '0' && check_next2(ls, "xX"))  /* hexadecimal? */
    expo = "Pp";
  for (;;) {
    // 指数部分
    if (check_next2(ls, expo))  /* exponent part? */
      // 指数部分的符号
      check_next2(ls, "-+");  /* optional exponent sign */
    // 十六进制数字，保存
    if (lisxdigit(ls->current))
      save_and_next(ls);
    // 小数点，保存
    else if (ls->current == '.')
      save_and_next(ls);
    // 其他的字符就跳出来
    else break;
  }
  save(ls, '\0');
  // 转换成字符，不成功就会报语法错误
  if (luaO_str2num(luaZ_buffer(ls->buff), &obj) == 0)  /* format error? */
    lexerror(ls, "malformed number", TK_FLT);
  // 转换成整数
  if (ttisinteger(&obj)) {
    seminfo->i = ivalue(&obj);
    return TK_INT;
  }
  else {
    // 转换成浮点数
    lua_assert(ttisfloat(&obj));
    seminfo->r = fltvalue(&obj);
    return TK_FLT;
  }
}


/*
** skip a sequence '[=*[' or ']=*]'; if sequence is well formed, return
** its number of '='s; otherwise, return a negative number (-1 iff there
** are no '='s after initial bracket)
*/
// 跳过序列'[=*['或']=*]'；如果序列格式正确，返回它的'='数；否则，返回一个负数（如果在初始括号后没有'='就返回-1） 
// [===[或者]===]这种序列
static int skip_sep (LexState *ls) {
  int count = 0;
  int s = ls->current;
  lua_assert(s == '[' || s == ']');
  // 保存当前的字符到LexState的buff中，并取后面一个字符
  save_and_next(ls);
  while (ls->current == '=') {
    save_and_next(ls);
    count++;
  }
  return (ls->current == s) ? count : (-count) - 1;
}

// 处理长注释或者字符串
static void read_long_string (LexState *ls, SemInfo *seminfo, int sep) {
  int line = ls->linenumber;  /* initial line (for error message) */
  // 保存并跳过第二个'['
  save_and_next(ls);  /* skip 2nd '[' */
  // 如果从新的一行开始，需要增加行号
  if (currIsNewline(ls))  /* string starts with a newline? */
    inclinenumber(ls);  /* skip it */
 
  for (;;) {
    // 处理当前字符
    switch (ls->current) {
      // 文件结束了
      case EOZ: {  /* error */
        const char *what = (seminfo ? "string" : "comment");
        const char *msg = luaO_pushfstring(ls->L,
                     "unfinished long %s (starting at line %d)", what, line);
        // 提示语法错误
        lexerror(ls, msg, TK_EOS);
        break;  /* to avoid warnings */
      }
      // 找到了可能是注释结束的标志
      case ']': {
        // 跳过序列'[=*['或']=*]'； [===[  ]===]
        // 如果之间的等号数目相等，表示结束了
        if (skip_sep(ls) == sep) {
          // 跳过第二个]
          save_and_next(ls);  /* skip 2nd ']' */
          goto endloop;
        }
        break;
      }
      // 如果碰到换行符，需要增加行号
      case '\n': case '\r': {
        save(ls, '\n');
        // 处理换行符，增加行号
        inclinenumber(ls);
        if (!seminfo) luaZ_resetbuffer(ls->buff);  /* avoid wasting space */
        break;
      }
      default: {
        // 默认保存字符，然后处理下一个字符
        if (seminfo) save_and_next(ls);
        else next(ls);
      }
    }
  } endloop:
  // 保存注释内容
  if (seminfo)
    seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + (2 + sep),
                                     luaZ_bufflen(ls->buff) - 2*(2 + sep));
}

// 校验转义字符
static void esccheck (LexState *ls, int c, const char *msg) {
  if (!c) {
    if (ls->current != EOZ)
      save_and_next(ls);  /* add current to buffer for error message */
    lexerror(ls, msg, TK_STRING);
  }
}

// 将当前字符转换成16进制的数值
static int gethexa (LexState *ls) {
  save_and_next(ls);
  esccheck (ls, lisxdigit(ls->current), "hexadecimal digit expected");
  return luaO_hexavalue(ls->current);
}

// 读取16进制转义，就是\xFF转换成255
static int readhexaesc (LexState *ls) {
  // 将前一个字符转换成16进制的数值
  int r = gethexa(ls);
  // 将前一个作为高4位，后一个字符作为低4位
  r = (r << 4) + gethexa(ls);
  luaZ_buffremove(ls->buff, 2);  /* remove saved chars from buffer */
  return r;
}

// 读取utf8的转义
static unsigned long readutf8esc (LexState *ls) {
  unsigned long r;
  int i = 4;  /* chars to be removed: '\', 'u', '{', and first digit */
  // 跳过u
  save_and_next(ls);  /* skip 'u' */
  // 检查u后面是否紧接着大括号
  esccheck(ls, ls->current == '{', "missing '{'");
  // 必须有一个数字
  r = gethexa(ls);  /* must have at least one digit */
  // 读取所有的数字
  while ((save_and_next(ls), lisxdigit(ls->current))) {
    i++;
    r = (r << 4) + luaO_hexavalue(ls->current);
    // 数值不能超过UTF8对应的最大值
    esccheck(ls, r <= 0x10FFFF, "UTF-8 value too large");
  }
  // 然后紧接着的大括号
  esccheck(ls, ls->current == '}', "missing '}'");
  next(ls);  /* skip '}' */
  // 删除这次解析的字符
  luaZ_buffremove(ls->buff, i);  /* remove saved chars from buffer */
  return r;
}

// utf8的转义
static void utf8esc (LexState *ls) {
  char buff[UTF8BUFFSZ];
  int n = luaO_utf8esc(buff, readutf8esc(ls));
  for (; n > 0; n--)  /* add 'buff' to string */
    save(ls, buff[UTF8BUFFSZ - n]);
}

// 读取十进制转义字符（使用转义串 \ddd ， 这里的 ddd 是一到三个十进制数字）比如：用\10可以表示换行
static int readdecesc (LexState *ls) {
  int i;
  int r = 0;  /* result accumulator */
  // 1到3个十进制数字
  for (i = 0; i < 3 && lisdigit(ls->current); i++) {  /* read up to 3 digits */
    r = 10*r + ls->current - '0';
    save_and_next(ls);
  }
  // 结果不能超过255，因为这三个十进制的数表示一个字符
  esccheck(ls, r <= UCHAR_MAX, "decimal escape too large");
  luaZ_buffremove(ls->buff, i);  /* remove read digits from buffer */
  return r;
}

// 读取字符串（短字符串）
// 单引号开始的，就得单引号结束，
// 双引号开始的，就得双引号结束
static void read_string (LexState *ls, int del, SemInfo *seminfo) {
  save_and_next(ls);  /* keep delimiter (for error messages) */
  // del表示成对的字符
  while (ls->current != del) {
    switch (ls->current) {
      // 没找到结尾的del字符，就文件结束了
      case EOZ:
        // 直接语法错误
        lexerror(ls, "unfinished string", TK_EOS);
        break;  /* to avoid warnings */
      // 短字符串是不能跨行的，如果出现了换行符之前没有del字符，
      case '\n':
      case '\r':
        // 直接语法错误
        lexerror(ls, "unfinished string", TK_STRING);
        break;  /* to avoid warnings */
      // 出现了转义字符 ：
      // '\a' （响铃）， '\b' （退格）， '\f' （换页）， '\n' （换行）， '\r' （回车）， 
      // '\t' （横项制表）， '\v' （纵向制表）， '\\' （反斜杠）， '\"' （双引号）， 以及 '\'' (单引号)。
      case '\\': {  /* escape sequences */
        int c;  /* final character to be saved */
        save_and_next(ls);  /* keep '\\' for error messages */
        switch (ls->current) {
          case 'a': c = '\a'; goto read_save;
          case 'b': c = '\b'; goto read_save;
          case 'f': c = '\f'; goto read_save;
          case 'n': c = '\n'; goto read_save;
          case 'r': c = '\r'; goto read_save;
          case 't': c = '\t'; goto read_save;
          case 'v': c = '\v'; goto read_save;
          // Lua 中的字符串可以保存任意 8 位值，其中包括用 '\0' 表示的 0 。 一般而言，你可以用字符的数字值来表示这个字符。
          // 方式是用转义串 \xXX， 此处的 XX 必须是恰好两个字符的 16 进制数。 
          case 'x': c = readhexaesc(ls); goto read_save;
          // 对于用 UTF-8 编码的 Unicode 字符，你可以用 转义符 \u{XXX} 来表示 （这里必须有一对花括号）， 此处的 XXX 是用 16 进制表示的字符编号
          case 'u': utf8esc(ls);  goto no_save;
          // 接一下行的符号，就是行尾的斜杠
          case '\n': case '\r':
            // 增加行号
            inclinenumber(ls); c = '\n'; goto only_save;
          // 转义 \ 、双引号和单引号
          case '\\': case '\"': case '\'':
            c = ls->current; goto read_save;
          // 到文件结尾了，下一个循环会报错
          case EOZ: goto no_save;  /* will raise an error next loop */
          // 转义串 '\z' 会忽略其后的一系列空白符，包括换行； 它在你需要对一个很长的字符串常量断行为多行并希望在每个新行保持缩进时非常有用。
          case 'z': {  /* zap following span of spaces */
            luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
            // 跳过z
            next(ls);  /* skip the 'z' */
            // 是否位空白字符 
            while (lisspace(ls->current)) {
              // 如果又换行符，需要增加行号
              if (currIsNewline(ls)) inclinenumber(ls);
              else next(ls);
            }
            goto no_save;
          }
          // 默认处理使用转义串\ddd ， 这里的 ddd 是一到三个十进制数字，
          // 这最多的三个数字组合起来表示一个字符，比如\10表示换行
          default: {
            // 如果当前字符不是数字，就报错
            esccheck(ls, lisdigit(ls->current), "invalid escape sequence");
            // 十进制转义字符串
            c = readdecesc(ls);  /* digital escape '\ddd' */
            goto only_save;
          }
        }
       // 读取下一个标签
       read_save:
         next(ls);
         /* go through */
       // 只是保存标签
       only_save:
         luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
         save(ls, c);
         /* go through */
       no_save: break;
      }
      default:
        // 默认就是保存，开始处理下一个字符
        save_and_next(ls);
    }
  }
  // 跳过结束符
  save_and_next(ls);  /* skip delimiter */
  // 将字符串保存起来
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                   luaZ_bufflen(ls->buff) - 2);
}

static int llex (LexState *ls, SemInfo *seminfo) {
  // 清空buff
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    // 解析当前读取的字符
    switch (ls->current) {
      // 如果是换行符，增加行号
      case '\n': case '\r': {  /* line breaks */
        inclinenumber(ls);
        break;
      }
      // 如果是空格，\f：换页符 \t：水平制表 \v：垂直制表，直接跳过到下一个
      case ' ': case '\f': case '\t': case '\v': {  /* spaces */
        // 直接进入下一个
        next(ls);
        break;
      }
      // 如果是 '-'
      case '-': {  /* '-' or '--' (comment) */
        // 得到下一个字符
        next(ls);
        // 判定是否也是'-'，不是直接返回，如果是表示注释了，
        if (ls->current != '-') return '-';
        // 再取下一个字符
        /* else is a comment */
        next(ls);
        // 进入长注释
        if (ls->current == '[') {  /* long comment? */
          // 跳过序列
          int sep = skip_sep(ls);
          luaZ_resetbuffer(ls->buff);  /* 'skip_sep' may dirty the buffer */
          // 确实是长注释
          if (sep >= 0) {
			// 处理长注释
            read_long_string(ls, NULL, sep);  /* skip long comment */
            luaZ_resetbuffer(ls->buff);  /* previous call may dirty the buff. */
            break;
          }
        }
        /* else short comment */
        // 短注释，处理到这一行结束或者文件结尾
        while (!currIsNewline(ls) && ls->current != EOZ)
          next(ls);  /* skip until end of line (or end of file) */
        break;
      }
      // 可能是长字符串或者就是一个单一的[
      case '[': {  /* long string or simply '[' */
        int sep = skip_sep(ls);
        // 如果长字符串
        if (sep >= 0) {
          read_long_string(ls, seminfo, sep);
          return TK_STRING;
        }
        // [后面跟着一个或者多个=却又没用第二个[,直接报语法错误
        else if (sep != -1)  /* '[=...' missing second bracket */
          lexerror(ls, "invalid long string delimiter", TK_STRING);
        return '[';
      }
      // 可能是 = 或者 == 
      case '=': {
        next(ls);
        // 如果是两个连续的==，表明比较Token
        if (check_next1(ls, '=')) return TK_EQ;
        else return '=';
      }
      // 可能是 < ， <= 或者 <<
      case '<': {
        next(ls);
        // 小于等于
        if (check_next1(ls, '=')) return TK_LE;
        // 左移
        else if (check_next1(ls, '<')) return TK_SHL;
        else return '<';
      }
      // 可能是 > ，>= 或者 >>
      case '>': {
        next(ls);
        // 大于等于
        if (check_next1(ls, '=')) return TK_GE;
        // 右移
        else if (check_next1(ls, '>')) return TK_SHR;
        else return '>';
      }
      // 可能是 / 或者 //(整除运算符, 5//2 = 2)
      case '/': {
        next(ls);
        // 整除运算符
        if (check_next1(ls, '/')) return TK_IDIV;
        else return '/';
      }
      // 可能是 ~ 或者 ~=
      case '~': {
        next(ls);
        // 不等于
        if (check_next1(ls, '=')) return TK_NE;
        else return '~';
      }
      // 可能是 : 或者 ::
      case ':': {
        next(ls);
        // 双冒号
        if (check_next1(ls, ':')) return TK_DBCOLON;
        else return ':';
      }
      // 双引号或者单引号，读取短字符串
      case '"': case '\'': {  /* short literal strings */
        read_string(ls, ls->current, seminfo);
        return TK_STRING;
      }
      // 可能是一个点（.：比如取table的成员），两个点（..：连接符)，三个点(...：可变参数)，或者浮点数
      case '.': {  /* '.', '..', '...', or number */
        save_and_next(ls);
        // 如果接着还是点
        if (check_next1(ls, '.')) {
          // 如果接着还是点
          if (check_next1(ls, '.'))
            // 多个点的Token
            return TK_DOTS;   /* '...' */
          // 连接符Token
          else return TK_CONCAT;   /* '..' */
        }
        // 后面没用跟数字，就是单纯的点
        else if (!lisdigit(ls->current)) return '.';
        // 后面跟着数字，解析成数字了
        else return read_numeral(ls, seminfo);
      }
      // 数字
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': {
        // 读取数字
        return read_numeral(ls, seminfo);
      }
      // 文件结束
      case EOZ: {
        return TK_EOS;
      }
      // 默认
      default: {
        // 是字母,
        if (lislalpha(ls->current)) {  /* identifier or reserved word? */
          TString *ts;
          do {
            save_and_next(ls);
          } while (lislalnum(ls->current));
          // 连续的内容创建一个新的字符串
          ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                  luaZ_bufflen(ls->buff));
          seminfo->ts = ts;
          // 是否为保留字
          if (isreserved(ts))  /* reserved word? */
            return ts->extra - 1 + FIRST_RESERVED;
          else {
            // 表示变量名
            return TK_NAME;
          }
        }
        // 一个单字符的token，比如(+ - /等等)
        else {  /* single-char tokens (+ - / ...) */
          int c = ls->current;
          next(ls);
          return c;
        }
      }
    }
  }
}

// 取得下一个Token
void luaX_next (LexState *ls) {
  ls->lastline = ls->linenumber;
  // 如果有预读的Token，先处理
  if (ls->lookahead.token != TK_EOS) {  /* is there a look-ahead token? */
    // 将当前的Token设置为lookahead
    ls->t = ls->lookahead;  /* use this one */
    ls->lookahead.token = TK_EOS;  /* and discharge it */
  }
  else
    // 读取下一个Token
    ls->t.token = llex(ls, &ls->t.seminfo);  /* read next token */
}

// 提前预读的Token，返回类型
int luaX_lookahead (LexState *ls) {
  lua_assert(ls->lookahead.token == TK_EOS);
  // 读取下一个Token 
  ls->lookahead.token = llex(ls, &ls->lookahead.seminfo);
  return ls->lookahead.token;
}

