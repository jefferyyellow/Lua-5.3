/*
** $Id: lvm.c,v 2.268.1.1 2017/04/19 17:39:34 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#define lvm_c
#define LUA_CORE

#include "lprefix.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"


/* limit for table tag-method chains (to avoid loops) */
#define MAXTAGLOOP	2000



/*
** 'l_intfitsf' checks whether a given integer can be converted to a
** float without rounding. Used in comparisons. Left undefined if
** all integers fit in a float precisely.
*/
#if !defined(l_intfitsf)

/* number of bits in the mantissa of a float */
#define NBM		(l_mathlim(MANT_DIG))

/*
** Check whether some integers may not fit in a float, that is, whether
** (maxinteger >> NBM) > 0 (that implies (1 << NBM) <= maxinteger).
** (The shifts are done in parts to avoid shifting by more than the size
** of an integer. In a worst case, NBM == 113 for long double and
** sizeof(integer) == 32.)
*/
#if ((((LUA_MAXINTEGER >> (NBM / 4)) >> (NBM / 4)) >> (NBM / 4)) \
	>> (NBM - (3 * (NBM / 4))))  >  0

#define l_intfitsf(i)  \
  (-((lua_Integer)1 << NBM) <= (i) && (i) <= ((lua_Integer)1 << NBM))

#endif

#endif



/*
** Try to convert a value to a float. The float case is already handled
** by the macro 'tonumber'.
*/
// 尝试将一个值转换成浮点数。就是tonumber的调用
int luaV_tonumber_ (const TValue *obj, lua_Number *n) {
  TValue v;
  // 如果是整数，直接转换就行
  if (ttisinteger(obj)) {
    *n = cast_num(ivalue(obj));
    return 1;
  }
  // 如果是字符串，转换成数字
  else if (cvt2num(obj) &&  /* string convertible to number? */
            luaO_str2num(svalue(obj), &v) == vslen(obj) + 1) {
    *n = nvalue(&v);  /* convert result of 'luaO_str2num' to a float */
    return 1;
  }
  else
    return 0;  /* conversion failed */
}


/*
** try to convert a value to an integer, rounding according to 'mode':
** mode == 0: accepts only integral values
** mode == 1: takes the floor of the number
** mode == 2: takes the ceil of the number
*/
// 试着将一个值转换成整数，
int luaV_tointeger (const TValue *obj, lua_Integer *p, int mode) {
  TValue v;
again:
  // obj是浮点数，根据模式得到，如果
  // mode为0，只接受整数，
  // mode为1，得到floor(n)
  // mode为2，表示ceil(n)
  if (ttisfloat(obj)) {
    lua_Number n = fltvalue(obj);
    lua_Number f = l_floor(n);
    if (n != f) {  /* not an integral value? */
      if (mode == 0) return 0;  /* fails if mode demands integral value */
      else if (mode > 1)  /* needs ceil? */
        f += 1;  /* convert floor to ceil (remember: n != f) */
    }
    return lua_numbertointeger(f, p);
  }
  // obj是整数类型，直接简单取值就行
  else if (ttisinteger(obj)) {
    *p = ivalue(obj);
    return 1;
  }
  // obj是字符串类型，
  else if (cvt2num(obj) &&
            luaO_str2num(svalue(obj), &v) == vslen(obj) + 1) {
    obj = &v;
	// 从字符串里转换成int和float，回到函数开始处，重新来过
    goto again;  /* convert result from 'luaO_str2num' to an integer */
  }
  return 0;  /* conversion failed */
}


/*
** Try to convert a 'for' limit to an integer, preserving the
** semantics of the loop.
** (The following explanation assumes a non-negative step; it is valid
** for negative steps mutatis mutandis.)
** If the limit can be converted to an integer, rounding down, that is
** it.
** Otherwise, check whether the limit can be converted to a number.  If
** the number is too large, it is OK to set the limit as LUA_MAXINTEGER,
** which means no limit.  If the number is too negative, the loop
** should not run, because any initial integer value is larger than the
** limit. So, it sets the limit to LUA_MININTEGER. 'stopnow' corrects
** the extreme case when the initial value is LUA_MININTEGER, in which
** case the LUA_MININTEGER limit would still run the loop once.
*/
// 尝试将for循环语句的限制转换为整数，保留循环的语意。（下面的解释假定一个非负的步长；
// 对于负数步长需要作必要的小修改才能奏效）
// 如果限制能转换为整数，将其向下取整。否则，查看限制释放能转换为浮点数。如果数字太大，
// 将其限制为 LUA_MAXINTEGER，表示没用限制。如果该值为绝对值最大的负数，循环就不应该运行，
// 因为任何初始化整数的值都比限制大。因此将其设置为LUA_MININTEGER。stopnow纠正了初始值为
// LUA_MININTEGER的极限情况，在LUA_MININTEGER为限制的情况下，仍然会运行一次循环。
static int forlimit (const TValue *obj, lua_Integer *p, lua_Integer step,
                     int *stopnow) {
  // 一般情况下不会设置stopnow,
  *stopnow = 0;  /* usually, let loops run */
  // 不能转换为整数
  if (!luaV_tointeger(obj, p, (step < 0 ? 2 : 1))) {  /* not fit in integer? */
    lua_Number n;  /* try to convert to float */
    // 不能转换为浮点数，就不是一个数字，返回0
    if (!tonumber(obj, &n)) /* cannot convert to float? */
      return 0;  /* not a number */
    // 如果浮点数大于最大的整数
    if (luai_numlt(0, n)) {  /* if true, float is larger than max integer */
      *p = LUA_MAXINTEGER;
      // 如果步长为负数，只运行一次循环
      if (step < 0) *stopnow = 1;
    }
    else {  /* float is smaller than min integer */
      // 如果浮点数小于最小的整数
      *p = LUA_MININTEGER;
      // 如果步长大于等于0，只运行一次循环
      if (step >= 0) *stopnow = 1;
    }
  }
  // 转换成功，返回1
  return 1;
}


/*
** Finish the table access 'val = t[key]'.
** if 'slot' is NULL, 't' is not a table; otherwise, 'slot' points to
** t[k] entry (which must be nil).
*/
// 完成表的访问'val = t[key]'
// 如果'slot'是NULL,那么t就不是table,或者slot就指向t[k]（t[k]必须是nil)
void luaV_finishget (lua_State *L, const TValue *t, TValue *key, StkId val,
                      const TValue *slot) {
  int loop;  /* counter to avoid infinite loops */
  const TValue *tm;  /* metamethod */
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
	// 如果t不是table
    if (slot == NULL) {  /* 't' is not a table? */
      lua_assert(!ttistable(t));
	  // 取得t的元方法
      tm = luaT_gettmbyobj(L, t, TM_INDEX);
	  // 没有元方法就直接报错
      if (ttisnil(tm))
        luaG_typeerror(L, t, "index");  /* no metamethod */
      /* else will try the metamethod */
    }
    else {  /* 't' is a table */
	  // t是个表
      lua_assert(ttisnil(slot));
	  // 从表里面取得元方法,如果没有就返回nil,结果就是取t[k]得到nil
      tm = fasttm(L, hvalue(t)->metatable, TM_INDEX);  /* table's metamethod */
      if (tm == NULL) {  /* no metamethod? */
        setnilvalue(val);  /* result is nil */
        return;
      }
      /* else will try the metamethod */
    }
	// 如果是一个函数，就调用，表示是一个元方法
    if (ttisfunction(tm)) {  /* is metamethod a function? */
      luaT_callTM(L, tm, t, key, val, 1);  /* call it */
      return;
    }
	// 元表还有元表，可以嵌套很多层，所以这是个循环去，最多MAXTAGLOOP
    t = tm;  /* else try to access 'tm[key]' */
    if (luaV_fastget(L,t,key,slot,luaH_get)) {  /* fast track? */
      setobj2s(L, val, slot);  /* done */
      return;
    }
    /* else repeat (tail call 'luaV_finishget') */
  }
  luaG_runerror(L, "'__index' chain too long; possible loop");
}


/*
** Finish a table assignment 't[key] = val'.
** If 'slot' is NULL, 't' is not a table.  Otherwise, 'slot' points
** to the entry 't[key]', or to 'luaO_nilobject' if there is no such
** entry.  (The value at 'slot' must be nil, otherwise 'luaV_fastset'
** would have done the job.)
*/
// 完成表的赋值't[key] = val'
// 如果slot是NULL，t就不是一个table，否则，slot指向一个条目t[key]，如果没有该条目就是luaO_nilobject
// 其实t[key]肯定是nil，否则的话luaV_fastset就能把完成赋值了
void luaV_finishset (lua_State *L, const TValue *t, TValue *key,
                     StkId val, const TValue *slot) {
  int loop;  /* counter to avoid infinite loops */
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;  /* '__newindex' metamethod */
    if (slot != NULL) {  /* is 't' a table? */
      Table *h = hvalue(t);  /* save 't' table */
      lua_assert(ttisnil(slot));  /* old value must be nil */
	  // 直接取原表
      tm = fasttm(L, h->metatable, TM_NEWINDEX);  /* get metamethod */
	  // 如果没有元方法也就不需要尝试元方法了，直接创建一个条目
      if (tm == NULL) {  /* no metamethod? */
        if (slot == luaO_nilobject)  /* no previous entry? */
          slot = luaH_newkey(L, h, key);  /* create one */
        /* no metamethod and (now) there is an entry with given key */
        setobj2t(L, cast(TValue *, slot), val);  /* set its new value */
        invalidateTMcache(h);
        luaC_barrierback(L, h, val);
        return;
      }
      /* else will try the metamethod */
    }
    else {  /* not a table; check metamethod */
		// 如果t不是表，那就取元方法，只能依赖元方法，不能像表一样创建条目
      if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
        luaG_typeerror(L, t, "index");
    }
    /* try the metamethod */
	// 如果是函数就调用函数
    if (ttisfunction(tm)) {
      luaT_callTM(L, tm, t, key, val, 0);
      return;
    }
	// 元方法可以嵌套元方法
    t = tm;  /* else repeat assignment over 'tm' */
    if (luaV_fastset(L, t, key, slot, luaH_get, val))
      return;  /* done */
    /* else loop */
  }
  luaG_runerror(L, "'__newindex' chain too long; possible loop");
}


/*
** Compare two strings 'ls' x 'rs', returning an integer smaller-equal-
** -larger than zero if 'ls' is smaller-equal-larger than 'rs'.
** The code is a little tricky because it allows '\0' in the strings
** and it uses 'strcoll' (to respect locales) for each segments
** of the strings.
*/
static int l_strcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls);
  size_t ll = tsslen(ls);
  const char *r = getstr(rs);
  size_t lr = tsslen(rs);
  for (;;) {  /* for each segment */
    int temp = strcoll(l, r);
    if (temp != 0)  /* not equal? */
      return temp;  /* done */
    else {  /* strings are equal up to a '\0' */
      size_t len = strlen(l);  /* index of first '\0' in both strings */
      if (len == lr)  /* 'rs' is finished? */
        return (len == ll) ? 0 : 1;  /* check 'ls' */
      else if (len == ll)  /* 'ls' is finished? */
        return -1;  /* 'ls' is smaller than 'rs' ('rs' is not finished) */
      /* both strings longer than 'len'; go on comparing after the '\0' */
      len++;
      l += len; ll -= len; r += len; lr -= len;
    }
  }
}


/*
** Check whether integer 'i' is less than float 'f'. If 'i' has an
** exact representation as a float ('l_intfitsf'), compare numbers as
** floats. Otherwise, if 'f' is outside the range for integers, result
** is trivial. Otherwise, compare them as integers. (When 'i' has no
** float representation, either 'f' is "far away" from 'i' or 'f' has
** no precision left for a fractional part; either way, how 'f' is
** truncated is irrelevant.) When 'f' is NaN, comparisons must result
** in false.
*/
static int LTintfloat (lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
  if (!l_intfitsf(i)) {
    if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
      return 1;  /* f >= maxint + 1 > i */
    else if (f > cast_num(LUA_MININTEGER))  /* minint < f <= maxint ? */
      return (i < cast(lua_Integer, f));  /* compare them as integers */
    else  /* f <= minint <= i (or 'f' is NaN)  -->  not(i < f) */
      return 0;
  }
#endif
  return luai_numlt(cast_num(i), f);  /* compare them as floats */
}


/*
** Check whether integer 'i' is less than or equal to float 'f'.
** See comments on previous function.
*/
static int LEintfloat (lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
  if (!l_intfitsf(i)) {
    if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
      return 1;  /* f >= maxint + 1 > i */
    else if (f >= cast_num(LUA_MININTEGER))  /* minint <= f <= maxint ? */
      return (i <= cast(lua_Integer, f));  /* compare them as integers */
    else  /* f < minint <= i (or 'f' is NaN)  -->  not(i <= f) */
      return 0;
  }
#endif
  return luai_numle(cast_num(i), f);  /* compare them as floats */
}


/*
** Return 'l < r', for numbers.
*/
static int LTnum (const TValue *l, const TValue *r) {
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li < ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LTintfloat(li, fltvalue(r));  /* l < r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numlt(lf, fltvalue(r));  /* both are float */
    else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
      return 0;  /* NaN < i is always false */
    else  /* without NaN, (l < r)  <-->  not(r <= l) */
      return !LEintfloat(ivalue(r), lf);  /* not (r <= l) ? */
  }
}


/*
** Return 'l <= r', for numbers.
*/
static int LEnum (const TValue *l, const TValue *r) {
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li <= ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LEintfloat(li, fltvalue(r));  /* l <= r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numle(lf, fltvalue(r));  /* both are float */
    else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
      return 0;  /*  NaN <= i is always false */
    else  /* without NaN, (l <= r)  <-->  not(r < l) */
      return !LTintfloat(ivalue(r), lf);  /* not (r < l) ? */
  }
}


/*
** Main operation less than; return 'l < r'.
*/
// 小于的主要操作，返回'l < r'
int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  // 数字
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LTnum(l, r);
  // 字符串
  else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) < 0;
  // 尝试调用元方法
  else if ((res = luaT_callorderTM(L, l, r, TM_LT)) < 0)  /* no metamethod? */
    luaG_ordererror(L, l, r);  /* error */
  return res;
}


/*
** Main operation less than or equal to; return 'l <= r'. If it needs
** a metamethod and there is no '__le', try '__lt', based on
** l <= r iff !(r < l) (assuming a total order). If the metamethod
** yields during this substitution, the continuation has to know
** about it (to negate the result of r<l); bit CIST_LEQ in the call
** status keeps that information.
*/
// 小于等于的主要操作；return 'l <= r',如果他需要一个元方法并且没有__le，试着使用__lt
// 基于 l <= r (r < l)(假设在一个总的顺序下）
int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  // 数字比较
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LEnum(l, r);
  // 字符串比较
  else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) <= 0;
  // le的元方法比较
  else if ((res = luaT_callorderTM(L, l, r, TM_LE)) >= 0)  /* try 'le' */
    return res;
  else {  /* try 'lt': */
	// 尝试 r < l,如果可以的话就把结果取反，得到了l <= r
    L->ci->callstatus |= CIST_LEQ;  /* mark it is doing 'lt' for 'le' */
	// 尝试r < l
    res = luaT_callorderTM(L, r, l, TM_LT);
    L->ci->callstatus ^= CIST_LEQ;  /* clear mark */
    if (res < 0)
      luaG_ordererror(L, l, r);
	// 将结果取反
    return !res;  /* result is negated */
  }
}


/*
** Main operation for equality of Lua values; return 't1 == t2'.
** L == NULL means raw equality (no metamethods)
*/
// lua值相等的主要操作，返回't1 == t2'的结果
int luaV_equalobj (lua_State *L, const TValue *t1, const TValue *t2) {
  const TValue *tm;
  // t1和t2的类型不一样
  if (ttype(t1) != ttype(t2)) {  /* not the same variant? */
	// 原始的类型也不一样，或者t1和t2的原始类型一致，但是t1不是数字类型的(没法直接比较)，直接返回0
    if (ttnov(t1) != ttnov(t2) || ttnov(t1) != LUA_TNUMBER)
      return 0;  /* only numbers can be equal with different variants */
	// 原始类型一样
    else {  /* two numbers with different variants */
      lua_Integer i1, i2;  /* compare them as integers */
      return (tointeger(t1, &i1) && tointeger(t2, &i2) && i1 == i2);
    }
  }
  /* values have same type and same variant */
  switch (ttype(t1)) {
    case LUA_TNIL: return 1;
    case LUA_TNUMINT: return (ivalue(t1) == ivalue(t2));
    case LUA_TNUMFLT: return luai_numeq(fltvalue(t1), fltvalue(t2));
    case LUA_TBOOLEAN: return bvalue(t1) == bvalue(t2);  /* true must be 1 !! */
    case LUA_TLIGHTUSERDATA: return pvalue(t1) == pvalue(t2);
    case LUA_TLCF: return fvalue(t1) == fvalue(t2);
    case LUA_TSHRSTR: return eqshrstr(tsvalue(t1), tsvalue(t2));			// 注意短字符串和长字符串的比较方式不同，短字符串只需要比较指向字符串的指针相等即可，因为短字符串用的是缓存机制
    case LUA_TLNGSTR: return luaS_eqlngstr(tsvalue(t1), tsvalue(t2));		// 长字符串先比较指针是否相同，如果不相同才比较两个字符串的内容是否相同
    case LUA_TUSERDATA: {
		// UserData，先直接比较，如果相等直接返回1，否则取取t1的等于比较的元方法，t1没有对应的元方法，取t2的等于比较的元方法，
      if (uvalue(t1) == uvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = fasttm(L, uvalue(t1)->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, uvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    case LUA_TTABLE: {
		// Table，先直接比较，如果相等直接返回1，否则取取t1的等于比较的元方法，t1没有对应的元方法，取t2的等于比较的元方法，
      if (hvalue(t1) == hvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = fasttm(L, hvalue(t1)->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, hvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    default:
      return gcvalue(t1) == gcvalue(t2);
  }
  if (tm == NULL)  /* no TM? */
    return 0;  /* objects are different */
  // 使用元方法比较
  luaT_callTM(L, tm, t1, t2, L->top, 1);  /* call TM */
  return !l_isfalse(L->top);
}


/* macro used by 'luaV_concat' to ensure that element at 'o' is a string */
#define tostring(L,o)  \
	(ttisstring(o) || (cvt2str(o) && (luaO_tostring(L, o), 1)))

#define isemptystr(o)	(ttisshrstring(o) && tsvalue(o)->shrlen == 0)

/* copy strings in stack from top - n up to top - 1 to buffer */
// 拷贝堆栈中的字符串，从top - n到top - 1字符串值拷贝到缓冲区，主要top - n其实是字符串指针
static void copy2buff (StkId top, int n, char *buff) {
  size_t tl = 0;  /* size already copied */
  do {
    size_t l = vslen(top - n);  /* length of string being copied */
    memcpy(buff + tl, svalue(top - n), l * sizeof(char));
    tl += l;
  } while (--n > 0);
}


/*
** Main operation for concatenation: concat 'total' values in the stack,
** from 'L->top - total' up to 'L->top - 1'.
*/
// 字符串连接的主要操作
void luaV_concat (lua_State *L, int total) {
  lua_assert(total >= 2);
  do {
    StkId top = L->top;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
	// top-2和top-1不是string，也不是可以转换成字符串的数字类型
    if (!(ttisstring(top-2) || cvt2str(top-2)) || !tostring(L, top-1))
      luaT_trybinTM(L, top-2, top-1, top-2, TM_CONCAT);
	// 栈顶的倒数第一值为空串
    else if (isemptystr(top - 1))  /* second operand is empty? */
      cast_void(tostring(L, top - 2));  /* result is first operand */
	// 栈顶的倒数第二值为空串
    else if (isemptystr(top - 2)) {  /* first operand is an empty string? */
      setobjs2s(L, top - 2, top - 1);  /* result is second op. */
    }
    else {
      /* at least two non-empty string values; get as many as possible */
      size_t tl = vslen(top - 1);
      TString *ts;
      /* collect total length and number of strings */
      for (n = 1; n < total && tostring(L, top - n - 1); n++) {
        size_t l = vslen(top - n - 1);
        if (l >= (MAX_SIZE/sizeof(char)) - tl)
          luaG_runerror(L, "string length overflow");
        tl += l;
      }
      if (tl <= LUAI_MAXSHORTLEN) {  /* is result a short string? */
        char buff[LUAI_MAXSHORTLEN];
        copy2buff(top, n, buff);  /* copy strings to buffer */
        ts = luaS_newlstr(L, buff, tl);
      }
      else {  /* long string; copy strings directly to final result */
        ts = luaS_createlngstrobj(L, tl);
        copy2buff(top, n, getstr(ts));
      }
      setsvalue2s(L, top - n, ts);  /* create result */
    }
	// 合并了 n - 1
    total -= n-1;  /* got 'n' strings to create 1 new */
	// 留一个保存结果
    L->top -= n-1;  /* popped 'n' strings and pushed one */
	// 一直循环直到只有一个结果留下来
  } while (total > 1);  /* repeat until only 1 result left */
}


/*
** Main operation 'ra' = #rb'.
*/
// 主要的操作为'ra' = '#rb',得到rb的长度
void luaV_objlen (lua_State *L, StkId ra, const TValue *rb) {
  const TValue *tm;
  switch (ttype(rb)) {
    case LUA_TTABLE: {
      Table *h = hvalue(rb);
	  // 如果是表的话，优先调用取长度的元方法
      tm = fasttm(L, h->metatable, TM_LEN);
      if (tm) break;  /* metamethod? break switch to call it */
	  // 如果原方法不存在，调用luaH_getn，注意：luaH_getn对于有空洞的表不一定准确
      setivalue(ra, luaH_getn(h));  /* else primitive len */
      return;
    }
    case LUA_TSHRSTR: {
		// 短字符串，直接取长度
      setivalue(ra, tsvalue(rb)->shrlen);
      return;
    }
    case LUA_TLNGSTR: {
		// 长字符串，直接取长度
      setivalue(ra, tsvalue(rb)->u.lnglen);
      return;
    }
    default: {  /* try metamethod */
		// 尝试原方法
      tm = luaT_gettmbyobj(L, rb, TM_LEN);
      if (ttisnil(tm))  /* no metamethod? */
        luaG_typeerror(L, rb, "get length of");
      break;
    }
  }
  luaT_callTM(L, tm, rb, rb, ra, 1);
}


/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
// 整数除法；返回 'm / n',并且是m/n以后舍去取整
lua_Integer luaV_div (lua_State *L, lua_Integer m, lua_Integer n) {
	// 特殊情况是:n为-1或者0
  if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to divide by zero");
    return intop(-, 0, m);   /* n==-1; avoid overflow with 0x80000...//-1 */
  }
  else {
    lua_Integer q = m / n;  /* perform C division */
	// 如果m / n为负数，并且不能整除，将q -= 1
    if ((m ^ n) < 0 && m % n != 0)  /* 'm/n' would be negative non-integer? */
      q -= 1;  /* correct result for different rounding */
    return q;
  }
}


/*
** Integer modulus; return 'm % n'. (Assume that C '%' with
** negative operands follows C99 behavior. See previous comment
** about luaV_div.)
*/
lua_Integer luaV_mod (lua_State *L, lua_Integer m, lua_Integer n) {
	// 特殊情况是:n为-1或者0
  if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to perform 'n%%0'");
    return 0;   /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
  }
  else {
    lua_Integer r = m % n;
	// 非负整数
    if (r != 0 && (m ^ n) < 0)  /* 'm/n' would be non-integer negative? */
      r += n;  /* correct result for different rounding */
    return r;
  }
}


/* number of bits in an integer */
// 一个整数的位数
#define NBITS	cast_int(sizeof(lua_Integer) * CHAR_BIT)

/*
** Shift left operation. (Shift right just negates 'y'.)
*/
// 左移位操作（右移位操作只是将y取负数）
lua_Integer luaV_shiftl (lua_Integer x, lua_Integer y) {
  if (y < 0) {  /* shift right? */
    if (y <= -NBITS) return 0;
    else return intop(>>, x, -y);
  }
  else {  /* shift left */
    if (y >= NBITS) return 0;
    else return intop(<<, x, y);
  }
}


/*
** check whether cached closure in prototype 'p' may be reused, that is,
** whether there is a cached closure with the same upvalues needed by
** new closure to be created.
*/
// 检查函数p中的闭包缓存是否可以重用，检查是否新创建的闭包是否和缓存的闭包有着相同的upvalues
static LClosure *getcached (Proto *p, UpVal **encup, StkId base) {
  LClosure *c = p->cache;
  if (c != NULL) {  /* is there a cached closure? */
    int nup = p->sizeupvalues;
    Upvaldesc *uv = p->upvalues;
    int i;
    for (i = 0; i < nup; i++) {  /* check whether it has right upvalues */
      TValue *v = uv[i].instack ? base + uv[i].idx : encup[uv[i].idx]->v;
      if (c->upvals[i]->v != v)
        return NULL;  /* wrong upvalue; cannot reuse closure */
    }
  }
  return c;  /* return cached closure (or NULL if no cached closure) */
}


/*
** create a new Lua closure, push it in the stack, and initialize
** its upvalues. Note that the closure is not cached if prototype is
** already black (which means that 'cache' was already cleared by the
** GC).
*/
// 创建一个新的lua闭包，将它压入堆栈并且初始化他的upvalues，
// 注意如果prototype没有置黑，将闭包缓存起来
static void pushclosure (lua_State *L, Proto *p, UpVal **encup, StkId base,
                         StkId ra) {
  int nup = p->sizeupvalues;
  Upvaldesc *uv = p->upvalues;
  int i;
  // 创建一个新的lua闭包
  LClosure *ncl = luaF_newLclosure(L, nup);
  ncl->p = p;
  setclLvalue(L, ra, ncl);  /* anchor new closure in stack */
  // 给upvales赋值
  for (i = 0; i < nup; i++) {  /* fill in its upvalues */
    // upvalue引用局部变量
    if (uv[i].instack)  /* upvalue refers to local variable? */
      ncl->upvals[i] = luaF_findupval(L, base + uv[i].idx);
    // 从包围函数得到upvalue
    else  /* get upvalue from enclosing function */
      ncl->upvals[i] = encup[uv[i].idx];
    ncl->upvals[i]->refcount++;
    /* new closure is white, so we do not need a barrier here */
  }
  // 如果不是black，缓存起来
  if (!isblack(p))  /* cache will not break GC invariant? */
    p->cache = ncl;  /* save it on cache for reuse */
}


/*
** finish execution of an opcode interrupted by an yield
*/
// 完成由yield中断的指令执行
void luaV_finishOp (lua_State *L) {
  CallInfo *ci = L->ci;
  StkId base = ci->u.l.base;
  Instruction inst = *(ci->u.l.savedpc - 1);  /* interrupted instruction */
  OpCode op = GET_OPCODE(inst);
  switch (op) {  /* finish its execution */
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
    case OP_MOD: case OP_POW:
    case OP_UNM: case OP_BNOT: case OP_LEN:
    case OP_GETTABUP: case OP_GETTABLE: case OP_SELF: {
      setobjs2s(L, base + GETARG_A(inst), --L->top);
      break;
    }
    case OP_LE: case OP_LT: case OP_EQ: {
      int res = !l_isfalse(L->top - 1);
      L->top--;
	  // 使用 < 来代替 <=(具体查看luaV_lessequal函数)
      if (ci->callstatus & CIST_LEQ) {  /* "<=" using "<" instead? */
        lua_assert(op == OP_LE);
        ci->callstatus ^= CIST_LEQ;  /* clear mark */
        res = !res;  /* negate result */
      }
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_JMP);
	  // 判断失败，跳过指令
      if (res != GETARG_A(inst))  /* condition failed? */
        ci->u.l.savedpc++;  /* skip jump instruction */
      break;
    }
    case OP_CONCAT: {
      StkId top = L->top - 1;  /* top when 'luaT_trybinTM' was called */
      int b = GETARG_B(inst);      /* first element to concatenate */
      int total = cast_int(top - 1 - (base + b));  /* yet to concatenate */
      setobj2s(L, top - 2, top);  /* put TM result in proper position */
      if (total > 1) {  /* are there elements to concat? */
        L->top = top - 1;  /* top is one after last element (at top-2) */
        luaV_concat(L, total);  /* concat them (may yield again) */
      }
      /* move final result to final position */
      setobj2s(L, ci->u.l.base + GETARG_A(inst), L->top - 1);
      L->top = ci->top;  /* restore top */
      break;
    }
    case OP_TFORCALL: {
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_TFORLOOP);
      L->top = ci->top;  /* correct top */
      break;
    }
    case OP_CALL: {
      if (GETARG_C(inst) - 1 >= 0)  /* nresults >= 0? */
        L->top = ci->top;  /* adjust results */
      break;
    }
    case OP_TAILCALL: case OP_SETTABUP: case OP_SETTABLE:
      break;
    default: lua_assert(0);
  }
}




/*
** {==================================================================
** Function 'luaV_execute': main interpreter loop
** ===================================================================
*/


/*
** some macros for common tasks in 'luaV_execute'
*/

// 从寄存器中取指令也就是在前面以R开头的宏中，实际代码中会使用一个base
// 再加上对应的地址，如：
// 寄存器
#define RA(i)	(base+GETARG_A(i))
#define RB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
#define RC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
// 寄存器或者常量
#define RKB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_B(i)) ? k+INDEXK(GETARG_B(i)) : base+GETARG_B(i))
#define RKC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_C(i)) ? k+INDEXK(GETARG_C(i)) : base+GETARG_C(i))


/* execute a jump instruction */
// 执行一个跳转指令
#define dojump(ci,i,e) \
  { int a = GETARG_A(i); \
    if (a != 0) luaF_close(L, ci->u.l.base + a - 1); \
    ci->u.l.savedpc += GETARG_sBx(i) + e; }

/* for test instructions, execute the jump instruction that follows it */
// 执行测试指令后面的跳转指令
#define donextjump(ci)	{ i = *ci->u.l.savedpc; dojump(ci, i, 1); }


#define Protect(x)	{ {x;}; base = ci->u.l.base; }

#define checkGC(L,c)  \
	{ luaC_condGC(L, L->top = (c),  /* limit of live values */ \
                         Protect(L->top = ci->top));  /* restore top */ \
           luai_threadyield(L); }


/* fetch an instruction and prepare its execution */
// 获取指令并准备执行
// 从保存的pc中取得下一条指令
#define vmfetch()	{ \
  i = *(ci->u.l.savedpc++); \
  if (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) \
    Protect(luaG_traceexec(L)); \
  ra = RA(i); /* WARNING: any stack reallocation invalidates 'ra' */ \
  lua_assert(base == ci->u.l.base); \
  lua_assert(base <= L->top && L->top < L->stack + L->stacksize); \
}

#define vmdispatch(o)	switch(o)
#define vmcase(l)	case l:
#define vmbreak		break


/*
** copy of 'luaV_gettable', but protecting the call to potential
** metamethod (which can reallocate the stack)
*/
#define gettableProtected(L,t,k,v)  { const TValue *slot; \
  if (luaV_fastget(L,t,k,slot,luaH_get)) { setobj2s(L, v, slot); } \
  else Protect(luaV_finishget(L,t,k,v,slot)); }


/* same for 'luaV_settable' */
// 
#define settableProtected(L,t,k,v) { const TValue *slot; \
  if (!luaV_fastset(L,t,k,slot,luaH_get,v)) \
    Protect(luaV_finishset(L,t,k,v,slot)); }



void luaV_execute (lua_State *L) {
  CallInfo *ci = L->ci;
  LClosure *cl;
  TValue *k;
  StkId base;
  ci->callstatus |= CIST_FRESH;  /* fresh invocation of 'luaV_execute" */
  // 帧改变时的重入点（调用/返回）
 newframe:  /* reentry point when frame changes (call/return) */
  lua_assert(ci == L->ci);
  // 函数闭包的局部引用
  cl = clLvalue(ci->func);  /* local reference to function's closure */
  // 对函数常量表的局部引用
  k = cl->p->k;  /* local reference to function's constant table */
  // 函数base的本地副本
  base = ci->u.l.base;  /* local copy of function's base */
  /* main loop of interpreter */
  // 解释器的主循环
  for (;;) {
    Instruction i;
    StkId ra;
    // 取指令
    vmfetch();
    // 根据不同的指令集，不同的处理
    vmdispatch (GET_OPCODE(i)) {
      // R(A) := R(B)	
      vmcase(OP_MOVE) {
        // 取得base+rb的值，赋值给ra
        setobjs2s(L, ra, RB(i));
        vmbreak;
      }
      // R(A) := Kst(Bx)
      vmcase(OP_LOADK) {
        // 从指令中得到bx的值，加上常量表的地址
        TValue *rb = k + GETARG_Bx(i);
        setobj2s(L, ra, rb);
        vmbreak;
      }
      // LOADKX是lua5.2新加入的指令。当需要生成LOADK指令时，如果需要索引的常量id超出了Bx所能表示的有效范围，
      // 那么就生成一个LOADKX指令，取代LOADK指令，并且接下来立即生成一个EXTRAARG指令，并用其
      // R(A) := Kst(extra arg)
      vmcase(OP_LOADKX) {
        TValue *rb;
        lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
        // 得到Ax的值，加上常量表基地址
        rb = k + GETARG_Ax(*ci->u.l.savedpc++);
        setobj2s(L, ra, rb);
        vmbreak;
      }
      // LOADBOOL将B所表示的boolean值装载到寄存器A中。B使用0和1分别代表false和true
      // R(A) := (Bool)B; if (C) pc++
      vmcase(OP_LOADBOOL) {
        setbvalue(ra, GETARG_B(i));
        // 如果C值不为0，那么PC寄存器自增1(跳过下一条指令)
        if (GETARG_C(i)) ci->u.l.savedpc++;  /* skip next instruction (if C) */
        vmbreak;
      }
      // OP_LOADNIL是可以同时对一段连续的寄存器都赋予nil值,所以，如果是对于连续的寄存器
      // 的赋值nil，lua编译器会合并到一个指令，而不连续的就不能合并，我们在编写的时候可
      // 以注意这个细节
      // R(A), R(A+1), ..., R(A+B) := nil
      vmcase(OP_LOADNIL) {
        // 从[a,a+b]这个区间的都设置为nil，前闭后闭区间
        int b = GETARG_B(i);
        do {
          setnilvalue(ra++);
        } while (b--);
        vmbreak;
      }
      // OP_GETUPVAL的B参数为Upvalue的索引，获取B参数对应的的Upvalue值，
      // 然后设置到A寄存器上。
      // R(A) := UpValue[B]	
      vmcase(OP_GETUPVAL) {
        int b = GETARG_B(i);
        setobj2s(L, ra, cl->upvals[b]->v);
        vmbreak;
      }
      // 从Upvalue列表中，获取指令B参数对应的Upvalue，
      // 并用指令C参数作为Key从Upvalue里面获取对应的值赋值给A寄存器。
      // R(A) := UpValue[B][RK(C)]
       vmcase(OP_GETTABUP) {
        // 从upvals数组中取得表
        TValue *upval = cl->upvals[GETARG_B(i)]->v;
        // 将指令C参数作为Key
        TValue *rc = RKC(i);
        gettableProtected(L, upval, rc, ra);
        vmbreak;
      }
      // R(A) := R(B)[RK(C)]		
      vmcase(OP_GETTABLE) {
        // 取指令B参数对应的寄存器的值为table
        StkId rb = RB(i);
        // C参数为key
        TValue *rc = RKC(i);
        gettableProtected(L, rb, rc, ra);
        vmbreak;
      }
      // UpValue[A][RK(B)] := RK(C)
      vmcase(OP_SETTABUP) {
        // 指令A参数为索引，得到upvals对应索引的值为table
        TValue *upval = cl->upvals[GETARG_A(i)]->v;
        // 从b参数得到key
        TValue *rb = RKB(i);
        // 从c参数得到值
        TValue *rc = RKC(i);
        // 对tab的key赋值
        settableProtected(L, upval, rb, rc);
        vmbreak;
      }
      // OP_GETUPVAL的B参数为Upvalue的索引，将A寄存器中的值设置B参数对应索引的的Upvalue值，
      // UpValue[B] := R(A)	
      vmcase(OP_SETUPVAL) {
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_upvalbarrier(L, uv);
        vmbreak;
      }
      // 寄存器A里面的table，通过B参数取得key,通过C参数取得value
      // R(A)[RK(B)] := RK(C)
      vmcase(OP_SETTABLE) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        settableProtected(L, ra, rb, rc);
        vmbreak;
      }
      // 创建一个表
      // R(A) := {} (size = B,C)		
      vmcase(OP_NEWTABLE) {
        // 参数b对应的寄存器里面保存了数组部分长度的一个转换数
        int b = GETARG_B(i);
        // 参数c对应的寄存器里面保存了hash部分长度的一个转换数
        int c = GETARG_C(i);
        // 创建一个table
        Table *t = luaH_new(L);
        sethvalue(L, ra, t);
        // 重新设置大小
        if (b != 0 || c != 0)
          luaH_resize(L, t, luaO_fb2int(b), luaO_fb2int(c));
        checkGC(L, ra + 1);
        vmbreak;
      }
      // R(A + 1) := R(B); R(A) := R(B)[RK(C)]
	  // 对于使用表的面向对象编程。 从表元素中检索函数引用并将其放入寄存器 R(A)，
      // 然后将对表本身的引用放入下一个寄存器 R(A+1)。 这条指令在设置方法调用时省去了一些繁琐的操作。
      // R(B) 是保存对带有方法的表的引用的寄存器。 方法函数本身是使用表索引 RK(C) 找到的，
      // 它可以是寄存器 R(C) 的值或常数。
      vmcase(OP_SELF) {
        const TValue *aux;
        StkId rb = RB(i);
        TValue *rc = RKC(i);
        // 转换成string
        TString *key = tsvalue(rc);  /* key must be a string */
        // 参数B取出的寄存器的值赋值给参数A后面的寄存器
        setobjs2s(L, ra + 1, rb);
        //  参数B取出的寄存器是table,参数C取出来的当key,取出来的值设置到A指向的寄存器
        if (luaV_fastget(L, rb, key, aux, luaH_getstr)) {
          setobj2s(L, ra, aux);
        }
        else Protect(luaV_finishget(L, rb, rc, ra, aux));
        vmbreak;
      }
      // R(A) := RK(B) + RK(C)
      // 将B，C 索引所对应的值相加放到寄存器A中，这里可以看到B,C可能是寄存器的索引也
      // 可能是常量表里的索引，在Lua中可以用ISK宏来判断是否是寄存器的值
      vmcase(OP_ADD) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // rb和rc都是整数，就按照整数+整数的方式
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(+, ib, ic));
        }
        // 将rb和rc转换为number，然后相加
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numadd(L, nb, nc));
        }
        // 尝试调用加的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_ADD)); }
        vmbreak;
      }
      // R(A) := RK(B) - RK(C)
	  // 将B，C 索引所对应的值相减放到寄存器A中，这里可以看到B,C可能是寄存器的索引也
	  // 可能是常量表里的索引，在Lua中可以用ISK宏来判断是否是寄存器的值
      vmcase(OP_SUB) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // rb和rc都是整数，就按照整数-整数的方式
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(-, ib, ic));
        }
        // 将rb和rc转换为number，然后相减
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numsub(L, nb, nc));
        }
        // 尝试调用减的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SUB)); }
        vmbreak;
      }
      // R(A) := RK(B) * RK(C)
	  // 将B，C 索引所对应的值相乘放到寄存器A中，这里可以看到B,C可能是寄存器的索引也
	  // 可能是常量表里的索引，在Lua中可以用ISK宏来判断是否是寄存器的值
      vmcase(OP_MUL) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // rb和rc都是整数，就按照整数 * 整数的方式
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(*, ib, ic));
        }
        // 将rb和rc转换为number，然后相乘
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_nummul(L, nb, nc));
        }
        // 尝试调用乘的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MUL)); }
        vmbreak;
      }
      // R(A) := RK(B) / RK(C)
      // 总是使用浮点除法，
	  // 将B，C 索引所对应的值相除放到寄存器A中，这里可以看到B,C可能是寄存器的索引也
	  // 可能是常量表里的索引，在Lua中可以用ISK宏来判断是否是寄存器的值
      vmcase(OP_DIV) {  /* float division (always with floats) */
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // 将rb和rc转换为number，然后相除
        if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numdiv(L, nb, nc));
        }
        // 尝试调用除的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_DIV)); }
        vmbreak;
      }
      // R(A) := RK(B) & RK(C) （按位与）操作，只有整数才能做按位与的操作
      vmcase(OP_BAND) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        // 将rb和rc转换为整数，然后按位与
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(&, ib, ic));
        }
        // 尝试调用按位与的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BAND)); }
        vmbreak;
      }
      // R(A) := RK(B) | RK(C)
      // 按位（或）操作，只有整数才能做按位或的操作
      vmcase(OP_BOR) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        // 将rb和rc转换为整数，然后按位或
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(|, ib, ic));
        }
        // 尝试调用按位或的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BOR)); }
        vmbreak;
      }
      // R(A) := RK(B) ~ RK(C)
      // （按位异或）操作,只有整数才能做按位异或的操作
      vmcase(OP_BXOR) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        // 将rb和rc转换为整数，然后按位异或
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(^, ib, ic));
        }
        // 尝试调用按位异或的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BXOR)); }
        vmbreak;
      }
      // R(A) := RK(B) << RK(C),左移）操作,只有整数才能做左移的操作
      vmcase(OP_SHL) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        // 将rb和rc转换为整数，然后左移
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, luaV_shiftl(ib, ic));
        }
        // 尝试调用左移的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHL)); }
        vmbreak;
      }
      // R(A) := RK(B) >> RK(C)，右移操作，只有整数才能做右移的操作
      vmcase(OP_SHR) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        // 将rb和rc转换为整数，然后右移
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, luaV_shiftl(ib, -ic));
        }
        // 尝试调用右移的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHR)); }
        vmbreak;
      }
      // R(A) := RK(B) % RK(C)
      vmcase(OP_MOD) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // 如果rb和rc为整数，转换后取模
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, luaV_mod(L, ib, ic));
        }
        // 如果rb和rc可以转换为number，进行浮点取模
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          lua_Number m;
          luai_nummod(L, nb, nc, m);
          setfltvalue(ra, m);
        }
        // 尝试调用取余的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MOD)); }
        vmbreak;
      }
      // R(A) := RK(B) // RK(C)  向下取整除法操作
      vmcase(OP_IDIV) {  /* floor division */
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // 如果rb和rc为整数，转换后相除
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, luaV_div(L, ib, ic));
        }
        // 如果rb和rc可以转换为number，进行浮点除法，然后再floor
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numidiv(L, nb, nc));
        }
        // 尝试调用向下取整除法的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_IDIV)); }
        vmbreak;
      }正
      // R(A) := RK(B) ^ RK(C) (^（次方）操作)
      vmcase(OP_POW) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        // 如果rb和rc可以转换为number，进行次方
        if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numpow(L, nb, nc));
        }
        // 尝试调用次方的元方法
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_POW)); }
        vmbreak;
      }
      // R(A) := -R(B) (-（取负）操作)
      vmcase(OP_UNM) {
        TValue *rb = RB(i);
        lua_Number nb;
        // 如果rb为整数，使用ra = 0 - rb的方式
        if (ttisinteger(rb)) {
          lua_Integer ib = ivalue(rb);
          setivalue(ra, intop(-, 0, ib));
        }
        // 如果rb能转换成number，直接取负号
        else if (tonumber(rb, &nb)) {
          setfltvalue(ra, luai_numunm(L, nb));
        }
        else {
          // 尝试调用取负的元方法
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_UNM));
        }
        vmbreak;
      }
      // R(A) := ~R(B)（按位非）操作,只有整数才能做按位非的操作
      vmcase(OP_BNOT) {
        TValue *rb = RB(i);
        lua_Integer ib;
        // 如果rb为整数，c语言里“^”按位异或运算符
        if (tointeger(rb, &ib)) {
          // ~l_castS2U(0)相当于得到所有位都为1的整数，然后和ib异或
          setivalue(ra, intop(^, ~l_castS2U(0), ib));
        }
        else {
          // 尝试调用按位非的元方法
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_BNOT));
        }
        vmbreak;
      }
      // R(A) := not R(B) 取反操作 not 总是返回 false 或 true 中的一个
      vmcase(OP_NOT) {
        TValue *rb = RB(i);
        // 是否为false，如果rb为true,就会返回false，如果rb为false，就会返回true,
        // 通过这种方式取反
        int res = l_isfalse(rb);  /* next assignment may change this value */
        setbvalue(ra, res);
        vmbreak;
      }
      // R(A) := length of R(B)(#（取长度）操作)
      vmcase(OP_LEN) {
        // 获取obj的长度
        Protect(luaV_objlen(L, ra, RB(i)));
        vmbreak;
      }
      // A B C	R(A) := R(B).. ... ..R(C)	（连接多个字符串）
      vmcase(OP_CONCAT) {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        StkId rb;
        // 设置栈顶
        L->top = base + c + 1;  /* mark the end of concat operands */
        // 设置连接字符串的数目
        Protect(luaV_concat(L, c - b + 1));
        // luaV_concat可能会调用元方法或移动栈
        ra = RA(i);  /* 'luaV_concat' may invoke TMs and move the stack */
        // 得到b的地址，连接后的元素最后在rb上
        rb = base + b;
        // 将rb = rb
        setobjs2s(L, ra, rb);
        checkGC(L, (ra >= rb ? ra + 1 : rb));
        // 恢复栈
        L->top = ci->top;  /* restore top */
        vmbreak;
      }
      // 跳转指令
      vmcase(OP_JMP) {
        // 执行跳转指令
        dojump(ci, i, 0);
        vmbreak;
      }
      // if ((RK(B) == RK(C)) ~= A) then pc++
      // 等于测试
      vmcase(OP_EQ) {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
		// 如果rb == rc的比较结果和ra不一致，不符合跳转的条件，执行下一条指令
        // 那就下一条指令
        Protect(
          if (luaV_equalobj(L, rb, rc) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            // 符合跳转的条件
            donextjump(ci);
        )
        vmbreak;
      }
      // if ((RK(B) <  RK(C)) ~= A) then pc++
      // 小于测试
      vmcase(OP_LT) {
        // 如果rb < rc的比较结果和ra不一致，不符合跳转的条件，执行下一条指令
        Protect(
          if (luaV_lessthan(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            // 符合条件的跳转
            donextjump(ci);
        )
        vmbreak;
      }
      // if ((RK(B) <= RK(C)) ~= A) then pc++
      // 小于等于测试
      vmcase(OP_LE) {
        Protect(
          // 如果rb <= rc的比较结果和ra不一致，不符合跳转的条件，执行下一条指令
          if (luaV_lessequal(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            // 符合条件的跳转
            donextjump(ci);
        )
        vmbreak;
      }
      // if not (R(A) <=> C) then pc++	
      // bool测试或者条件判定
      vmcase(OP_TEST) {
        // 根据rc的不同值来判定ra为false还是true，判定是否满足跳转，不满足就执行下一条指令
        if (GETARG_C(i) ? l_isfalse(ra) : !l_isfalse(ra))
            ci->u.l.savedpc++;
          else
          // 符合条件的跳转
          donextjump(ci);
        vmbreak;
      }
      // if (R(B) <=> C) then R(A) := R(B) else pc++	
      // bool测试或者条件判定
      vmcase(OP_TESTSET) {
        TValue *rb = RB(i);
        // 根据rc的不同值来判定ra为false还是true，判定是否满足跳转，不满足就执行下一条指
        if (GETARG_C(i) ? l_isfalse(rb) : !l_isfalse(rb))
          ci->u.l.savedpc++;
        else {
          // 满足条件将ra = rb，并且作符合条件的跳转
          setobjs2s(L, ra, rb);
          donextjump(ci);
        }
        vmbreak;
      }
      // R(A), ... ,R(A+C-2) := R(A)(R(A+1), ... ,R(A+B-1))
      // 
      vmcase(OP_CALL) {
        int b = GETARG_B(i);
        // rc保存了返回值数目
        int nresults = GETARG_C(i) - 1;
        // rb表示参数的的数目，如果b为0，表示上一条指令已经设置了top
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        // C 函数
        // ra表示函数
        if (luaD_precall(L, ra, nresults)) {  /* C function? */
          // 如果有返回值，调整返回值
          if (nresults >= 0)
            L->top = ci->top;  /* adjust results */
          Protect((void)0);  /* update 'base' */
        }
        // lua函数
        else {  /* Lua function */
          ci = L->ci;
          // 在新的Lua函数上重启 luaV_execute
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
        vmbreak;
      }
      // return R(A)(R(A+1), ... ,R(A+B-1)) 尾调用
      vmcase(OP_TAILCALL) {
        int b = GETARG_B(i);
        // rb表示参数的的数目，如果b为0，表示上一条指令已经设置了top
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        // 不定返回值
        lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
        // C函数调用
        if (luaD_precall(L, ra, LUA_MULTRET)) {  /* C function? */
          Protect((void)0);  /* update 'base' */
        }
        else {
          /* tail call: put called frame (n) in place of caller one (o) */
          // 尾调用：将被调用的帧 (n) 放在调用者一 (o) 的位置
          // 取出调用者和被调用者的栈帧和函数
          CallInfo *nci = L->ci;  /* called frame */
          CallInfo *oci = nci->previous;  /* caller frame */
          StkId nfunc = nci->func;  /* called function */
          StkId ofunc = oci->func;  /* caller function */
          /* last stack slot filled by 'precall' */
          // 最后一个栈槽被'precall'填充
          StkId lim = nci->u.l.base + getproto(nfunc)->numparams;
          int aux;
          /* close all upvalues from previous call */
          // 关闭上次调用的所有upvalues
          if (cl->p->sizep > 0) luaF_close(L, oci->u.l.base);
          /* move new frame into old one */
          // 移动新的栈帧到原来的里面
          for (aux = 0; nfunc + aux < lim; aux++)
            setobjs2s(L, ofunc + aux, nfunc + aux);
          // 修正调用者的栈和保存的指令地址
          oci->u.l.base = ofunc + (nci->u.l.base - nfunc);  /* correct base */
          oci->top = L->top = ofunc + (L->top - nfunc);  /* correct top */
          oci->u.l.savedpc = nci->u.l.savedpc;
          // 设置未尾调用模式
          oci->callstatus |= CIST_TAIL;  /* function was tail called */
          // 删除新的栈帧
          ci = L->ci = oci;  /* remove new frame */
          lua_assert(L->top == oci->u.l.base + getproto(ofunc)->maxstacksize);
          // 在新的Lua函数上重启 luaV_execute
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
        vmbreak;
      }
      // return R(A), ... ,R(A+B-2),返回指令
      vmcase(OP_RETURN) {
        int b = GETARG_B(i);
        // 关闭所有upvalues
        if (cl->p->sizep > 0) luaF_close(L, base);
        // 完成一个函数调用 
        b = luaD_poscall(L, ci, ra, (b != 0 ? b - 1 : cast_int(L->top - ra)));
        // 局部变量'ci'仍然来自被调用者
        if (ci->callstatus & CIST_FRESH)  /* local 'ci' still from callee */
          return;  /* external invocation: return */
        // 通过重入调用：继续执行
        else {  /* invocation via reentry: continue execution */
          ci = L->ci;
          if (b) L->top = ci->top;
          lua_assert(isLua(ci));
          lua_assert(GET_OPCODE(*((ci)->u.l.savedpc - 1)) == OP_CALL);
          // 在新的Lua函数上重启 luaV_execute
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      }
      // For循环
	  // R(A)+=R(A+2);
	  // if R(A) < ?= R(A + 1) then { pc += sBx; R(A + 3) = R(A) }*/
      vmcase(OP_FORLOOP) {
        // 整数循环
        if (ttisinteger(ra)) {  /* integer loop? */
          // 步长
          lua_Integer step = ivalue(ra + 2);
          // 循环子
          lua_Integer idx = intop(+, ivalue(ra), step); /* increment index */
          // 循环终止条件
          lua_Integer limit = ivalue(ra + 1);
          // 如果满足循环条件
          if ((0 < step) ? (idx <= limit) : (limit <= idx)) {
            // 跳回循环开始处
            ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
            // ra = idx, 更新内部索引
            chgivalue(ra, idx);  /* update internal index... */
            // (ra + 3) = idx, 更新外部索引
            setivalue(ra + 3, idx);  /* ...and external index */
          }
        }
        // 浮点数循环
        else {  /* floating loop */
          // 步长
          lua_Number step = fltvalue(ra + 2);
          // 循环子
          lua_Number idx = luai_numadd(L, fltvalue(ra), step); /* inc. index */
          // 循环终止条件
          lua_Number limit = fltvalue(ra + 1);
          // 满足循环条件
          if (luai_numlt(0, step) ? luai_numle(idx, limit)
                                  : luai_numle(limit, idx)) {
            // 跳回循环开始处
            ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
            // 更新内外部循环
            chgfltvalue(ra, idx);  /* update internal index... */
            setfltvalue(ra + 3, idx);  /* ...and external index */
          }
        }
        vmbreak;
      }
      // R(A)-=R(A+2); pc+=sBx	
      // FORPREP初始化一个数字for循环，而FORLOOP执行一个数字for循环的迭代。
      vmcase(OP_FORPREP) {
        // 初始值
        TValue *init = ra;
        // 循环终止值
        TValue *plimit = ra + 1;
        // 步长
        TValue *pstep = ra + 2;
        lua_Integer ilimit;
        // 如果stopnow为1表示限制转换有问题，与步长不匹配，只运行一次
        int stopnow;
        // 整数循环
        if (ttisinteger(init) && ttisinteger(pstep) &&
            forlimit(plimit, &ilimit, ivalue(pstep), &stopnow)) {
          /* all values are integer */
          lua_Integer initv = (stopnow ? 0 : ivalue(init));
          // 设置限制和初始值
          setivalue(plimit, ilimit);
          setivalue(init, intop(-, initv, ivalue(pstep)));
        }
        // 浮点数循环
        else {  /* try making all values floats */
            // 尝试将所有的值转换为浮点数
          lua_Number ninit; lua_Number nlimit; lua_Number nstep;
          if (!tonumber(plimit, &nlimit))
            luaG_runerror(L, "'for' limit must be a number");
          setfltvalue(plimit, nlimit);
          if (!tonumber(pstep, &nstep))
            luaG_runerror(L, "'for' step must be a number");
          setfltvalue(pstep, nstep);
          if (!tonumber(init, &ninit))
            luaG_runerror(L, "'for' initial value must be a number");
          // 设置初始值
          setfltvalue(init, luai_numsub(L, ninit, nstep));
        }
        ci->u.l.savedpc += GETARG_sBx(i);
        vmbreak;
      }
      // OP_TFORCALL和OP_TFORLOOP这个两个指令实现了泛型for循环
      // R(A+3), ... ,R(A+2+C) := R(A)(R(A+1), R(A+2));
      // 它是iABC模式，A域指定迭代器函数的位置，B域不使用，C则指明了从迭代器函数中接收多少个返回值。执行它的时候，
      // 如果我们的迭代器函数位于R(A)，那么此时，它会将迭代函数、迭代的table和当前迭代的key设置到
      // R(A+3)~R(A+5)的位置上，
      vmcase(OP_TFORCALL) {
        // ra+3调用的基准位置，拷贝一份出来
        StkId cb = ra + 3;  /* call base */
        setobjs2s(L, cb+2, ra+2);
        setobjs2s(L, cb+1, ra+1);
        // ra为迭代器函数
        setobjs2s(L, cb, ra);
        L->top = cb + 3;  /* func. + 2 args (state and index) */
        // 调用迭代器函数，rc表示返回值数目
        Protect(luaD_call(L, cb, GETARG_C(i)));
        L->top = ci->top;
        // 继续下一条指令
        i = *(ci->u.l.savedpc++);  /* go to next instruction */
        ra = RA(i);
        lua_assert(GET_OPCODE(i) == OP_TFORLOOP);
        goto l_tforloop;
      }
      // 泛型for循环
      // if R(A+1) ~= nil then { R(A)=R(A+1); pc += sBx }
      vmcase(OP_TFORLOOP) {
      l_tforloop:
        // 是否继续循环
        if (!ttisnil(ra + 1)) {  /* continue loop? */
          // 保存控制变量 ra= r(a+1)
          setobjs2s(L, ra, ra + 1);  /* save control variable */
          // 跳转回到循环开始处
           ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
        }
        vmbreak;
      }
      // R(A)[(C-1)*FPF+i] := R(A+i), 1 <= i <= B
      // 以一个基地址和数量来将数据写入表的数组部分
      vmcase(OP_SETLIST) {
        // rb为待写入数据的数量
        int n = GETARG_B(i);
        // rc为FPF（也就是前面提到的LFIELDS_PER_FLUSH常量）索引，即每次写入最多是LFIELDS_PER_FLUSH
        int c = GETARG_C(i);
        unsigned int last;
        Table *h;
        // 如果rb为零，直接取栈顶到ra的数目
        if (n == 0) n = cast_int(L->top - ra) - 1;
        // 如果c为0，表示数目比较大，需要从下一条指令取出数目
        if (c == 0) {
          lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
          c = GETARG_Ax(*ci->u.l.savedpc++);
        }
        // 从ra取出表
        h = hvalue(ra);
        // 得到最后需要设置得项
        last = ((c-1)*LFIELDS_PER_FLUSH) + n;
        // 需要更大得空间
        if (last > h->sizearray)  /* needs more space? */
          luaH_resizearray(L, h, last);  /* preallocate it at once */
        // 将值写入
        for (; n > 0; n--) {
          TValue *val = ra+n;
          luaH_setint(L, h, last--, val);
          luaC_barrierback(L, h, val);
        }
        // 修正堆栈
        L->top = ci->top;  /* correct top (in case of previous open call) */
        vmbreak;
      }
      // R(A) := closure(KPROTO[Bx])
      // 当遇到函数定义是，会在函数定义位置生成一个OP_CLOSURE指令，其第一个参数表示
      // 指令返回的Closure结构存放的寄存器编号，第二个参数表示该函数内定义的函数的编号。
      vmcase(OP_CLOSURE) {
        // 得到函数原型
        Proto *p = cl->p->p[GETARG_Bx(i)];
        // 取缓存的Lua闭包
        LClosure *ncl = getcached(p, cl->upvals, base);  /* cached closure */
        // 找不到匹配的缓存
        if (ncl == NULL)  /* no match? */
          // 创建一个新的闭包
          pushclosure(L, p, cl->upvals, base, ra);  /* create a new one */
        else
          // 设置lua闭包
          setclLvalue(L, ra, ncl);  /* push cashed closure */
        checkGC(L, ra + 1);
        vmbreak;
      }
      // R(A), R(A+1), ..., R(A+B-2) 
      // Lua为不定参数专门设计了一条指令：OP_VARARG A B ==> R(A), R(A+1), ..., R(A+B-2) = vararg，
      // 每一次代码用采用”…"操作符获取不定参数时，都会生成这条指令（固定参数的值可被直接修改，
      // 不定参数的值无法被直接修改，只能通过VARARG指令获取其副本）。另外，“…”操作符只有一个获取
      // 不定参数副本的功能，无法通过该操作符来接收多个返回值。
      vmcase(OP_VARARG) {
        // 要求的数目
        int b = GETARG_B(i) - 1;  /* required results */
        int j;
        // 不定参数的数目
        int n = cast_int(base - ci->func) - cl->p->numparams - 1;
        // 实际的参数比固定参数还少，没用可变参数
        if (n < 0)  /* less arguments than parameters? */
          n = 0;  /* no vararg arguments */
        // 如果b小于0，表示接收所有的不定参数
        if (b < 0) {  /* B == 0? */
          b = n;  /* get all var. arguments */
          // 栈空间是否能容纳n个参数
          Protect(luaD_checkstack(L, n));
          ra = RA(i);  /* previous call may change the stack */
          // 增加栈空间
          L->top = ra + n;
        }
        // 设置不定参数
        for (j = 0; j < b && j < n; j++)
          setobjs2s(L, ra + j, base - n + j);
        // 如果要求的参数比实际参数多，需要将不足的填充为nil
        for (; j < b; j++)  /* complete required results with nil */
          setnilvalue(ra + j);
        vmbreak;
      }
      vmcase(OP_EXTRAARG) {
        lua_assert(0);
        vmbreak;
      }
    }
  }
}

/* }================================================================== */

