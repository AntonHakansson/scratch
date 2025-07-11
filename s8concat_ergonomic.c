#if IN_SHELL /* $ bash s8concat_ergonomic.c
cc s8concat_ergonomic.c -o s8concat_ergonomic -fsanitize=undefined -g3 -Wall -Wextra -Wconversion -Wno-sign-conversion $@
exit # */
#endif

// Experimental pre-processor macro for ergonomic s8concat syntax.
//
// Compare
// Explicit version:
//
//  s8concat(arena, s8("if ("), s8var, s8(") {\n"),
//                  s8("  "), var_name, s8(" = "), value, s8(";\n");
//
// with ergonomic version (limited to 16 arguments):
//
//  s8concat(arena, "if (", s8var, ") {\n",
//                  "  ", var_name, " = ", value, ";\n")
//
//

#define tassert(c)       while (!(c)) __builtin_trap()
#define countof(a)       (Iz)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)     ((t *)arena_alloc(a, sizeof(t), _Alignof(t), (n)))
#define newbeg(a, t, n)  ((t *)arena_alloc_beg(a, sizeof(t), _Alignof(t), (n)))
#define memcpy(d, s, n)  __builtin_memcpy(d, s, n)
#define memset(d, c, n)  __builtin_memset(d, c, n)
#define strlen(s)        __builtin_strlen(s)

typedef unsigned char U8;
typedef signed long long I64;
typedef typeof((char *)0-(char *)0) Iz;
typedef typeof(sizeof(0))           Uz;

////////////////////////////////////////////////////////////////////////////////
//- Arena

typedef struct { U8 *beg; U8 *end; } Arena;

static U8 *arena_alloc(Arena *a, Iz objsize, Iz align, Iz count) {
  Iz padding = (Uz)a->end & (align - 1);
  tassert((count <= (a->end - a->beg - padding) / objsize) && "out of memory");
  Iz total = objsize * count;
  return memset(a->end -= total + padding, 0, total);
}

static U8 *arena_alloc_beg(Arena *a, Iz objsize, Iz align, Iz count) {
  Iz padding = -(Uz)(a->beg) & (align - 1);
  Iz total   = padding + objsize * count;
  tassert(total < (a->end - a->beg) && "out of memory");
  U8 *p = a->beg + padding;
  memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- String

#define s8(X) _Generic((X), default: s8x_iden_, char *: s8x_lit_)((X))
#define s8concat(arena, head, ...) \
  s8concatv(arena, s8(head), (S8[]){s8X_APPLY(__VA_ARGS__)}, countof((S8[]){s8X_APPLY(__VA_ARGS__)}));
#define s8pri(s) (int)s.len, s.data

typedef struct { U8 *data; Iz len; } S8;

static S8 s8concatv(Arena *a, S8 head, S8 *ss, Iz count) {
  S8 r = {0};
  if (!head.data || (U8 *)(head.data+head.len) != a->beg) {
    S8 copy = head;
    copy.data = newbeg(a, U8, head.len);
    if (head.len) memcpy(copy.data, head.data, head.len);
    head = copy;
  }
  for (Iz i = 0; i < count; i++) {
    S8 tail = ss[i];
    U8 *data = newbeg(a, U8, tail.len);
    if (tail.len) memcpy(data, tail.data, tail.len);
    head.len += tail.len;
  }
  r = head;
  return r;
}

// s8concat convenience monstrosity
static S8 s8x_iden_(S8 s) { return s; }
static S8 s8x_lit_(char *s) { return (S8){(U8 *)s, strlen(s)}; }
#define s8X_APPLY1(a) s8(a)
#define s8X_APPLY2(a, b) s8(a), s8(b)
#define s8X_APPLY3(a, b, c) s8(a), s8(b), s8(c)
#define s8X_APPLY4(a, b, c, d) s8(a), s8(b), s8(c), s8(d)
#define s8X_APPLY5(a, b, c, d, e) s8(a), s8(b), s8(c), s8(d), s8(e)
#define s8X_APPLY6(a, b, c, d, e, f) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f)
#define s8X_APPLY7(a, b, c, d, e, f, g) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g)
#define s8X_APPLY8(a, b, c, d, e, f, g, h) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h)
#define s8X_APPLY9(a, b, c, d, e, f, g, h, i) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i)
#define s8X_APPLY10(a, b, c, d, e, f, g, h, i, j) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j)
#define s8X_APPLY11(a, b, c, d, e, f, g, h, i, j, k) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k)
#define s8X_APPLY12(a, b, c, d, e, f, g, h, i, j, k, l) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k), s8(l)
#define s8X_APPLY13(a, b, c, d, e, f, g, h, i, j, k, l, m) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k), s8(l), s8(m)
#define s8X_APPLY14(a, b, c, d, e, f, g, h, i, j, k, l, m, n) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k), s8(l), s8(m), s8(n)
#define s8X_APPLY15(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k), s8(l), s8(m), s8(n), s8(o)
#define s8X_APPLY16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) s8(a), s8(b), s8(c), s8(d), s8(e), s8(f), s8(g), s8(h), s8(i), s8(j), s8(k), s8(l), s8(m), s8(n), s8(o), s8(p)
#define s8X_GET_MACRO(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,NAME,...) NAME
#define s8X_APPLY(...) s8X_GET_MACRO(__VA_ARGS__, s8X_APPLY16, s8X_APPLY15, s8X_APPLY14, s8X_APPLY13, s8X_APPLY12, s8X_APPLY11, s8X_APPLY10, s8X_APPLY9, s8X_APPLY8, s8X_APPLY7, s8X_APPLY6, s8X_APPLY5, s8X_APPLY4, s8X_APPLY3, s8X_APPLY2, s8X_APPLY1)(__VA_ARGS__)

static S8 s8i64(Arena *arena, I64 x) {
  _Bool negative = (x < 0);
  if (negative) { x = -x; }
  char digits[20];
  int i = countof(digits);
  do {
    digits[--i] = (char)(x % 10) + '0';
  } while (x /= 10);
  Iz len = countof(digits) - i + negative;
  U8 *beg = new(arena, U8, len);
  U8 *end = beg;
  if (negative) { *end++ = '-'; }
  do { *end++ = digits[i++]; } while (i < countof(digits));
  return (S8){beg, len};
}


#include <stdio.h>
#include <stdlib.h>
int main() {
  enum { HEAP_CAP = (1u << 28) };
  U8 *heap = malloc(HEAP_CAP);
  Arena arena[1] = { (Arena){heap, heap + HEAP_CAP}, };

  S8 var = s8("variable");
  S8 var_next = s8concat(arena, var, "->next");
  S8 i64_s = s8i64(arena, 42);
  S8 result = s8concat(arena, "The next pointer of ", var, " is ", var_next, " with value ", i64_s);
  printf("%.*s", s8pri(result));

  return 0;
}
