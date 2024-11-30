#if IN_SHELL /* $ bash generators.c
cc generators.c -o generators -fsanitize=undefined -g3 -Os -pedantic -Wall -Wextra -Wconversion -Wno-sign-conversion $@
exit # */
#endif

#define assert(c)        while (!(c)) __builtin_unreachable()
#define tassert(c)       while (!(c)) __builtin_trap()
#define breakpoint(c)    ((c) ? ({ asm volatile ("int3; nop"); }) : 0)
#define countof(a)       (Size)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)     ((t *)arena_alloc(a, sizeof(t), _Alignof(t), (n)))
#define newbeg(a, t, n)  ((t *)arena_alloc_beg(a, sizeof(t), _Alignof(t), (n)))
#define s8(s)            (S8){(U8 *)s, countof(s)-1}
#define memcpy(d, s, n)  __builtin_memcpy(d, s, n)
#define memset(d, c, n)  __builtin_memset(d, c, n)

typedef unsigned char U8;
typedef signed long long I64;
typedef typeof((char *)0-(char *)0) Size;
typedef typeof(sizeof(0))           USize;

////////////////////////////////////////////////////////////////////////////////
//- Arena

typedef struct { U8 *beg;  U8  *end; } Arena;

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = (USize)a->end & (align - 1);
  tassert((count <= (a->end - a->beg - padding) / objsize) && "out of memory");
  Size total = objsize * count;
  return memset(a->end -= total + padding, 0, total);
}

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static U8 *arena_alloc_beg(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
  tassert(total < (a->end - a->beg) && "out of memory");
  U8 *p = a->beg + padding;
  memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- String

#define s8pri(s) (int)s.len, s.data

typedef struct { U8 *data; Size len; } S8;

static S8 s8span(U8 *beg, U8 *end) { return (S8){beg, end - beg}; }

static S8 s8dup(Arena *a, S8 s) {
  return (S8) {
    memcpy((new(a, U8, s.len)), s.data, s.len),
    s.len,
  };
}

static char *s8z(Arena *a, S8 s) {
  return memcpy(new(a, char, s.len + 1), s.data, s.len);
}

static S8 s8cstr(Arena *a, char *cstr) {
  return s8dup(a, (S8){(U8*)cstr, __builtin_strlen(cstr)});
}

#define s8concat(arena, head, ...)                                                   \
  s8concatv(arena, head, ((S8[]){__VA_ARGS__}), (countof(((S8[]){__VA_ARGS__}))))

static S8 s8concatv(Arena *a, S8 head, S8 *ss, Size count) {
  S8 r = {0};
  if (!head.data || (U8 *)(head.data+head.len) != a->beg) {
    S8 copy = head;
    copy.data = newbeg(a, U8, head.len);
    if (head.len) memcpy(copy.data, head.data, head.len);
    head = copy;
  }
  for (Size i = 0; i < count; i++) {
    S8 tail = ss[i];
    U8 *data = newbeg(a, U8, tail.len);
    if (tail.len) memcpy(data, tail.data, tail.len);
    head.len += tail.len;
  }
  r = head;
  return r;
}

static S8 s8trimspace(S8 s) {
  for (Size off = 0; off < s.len; off++) {
    _Bool is_ws = (s.data[off] == ' ' || ((unsigned)s.data[off] - '\t') < 5);
    if (!is_ws) { return (S8){s.data + off, s.len - off}; }
  }
  return s;
}

static _Bool s8match(S8 a, S8 b, Size n) {
  if (a.len < n || b.len < n)  { return 0; }
  for (Size i = 0; i < n; i++) {
    if (a.data[i] != b.data[i]) { return 0; }
  }
  return 1;
}

#define s8startswith(a, b) s8match((a), (b), (b).len)

static S8 s8tolower(S8 s) {
  for (Size i = 0; i < s.len; i++) {
    if (((unsigned)s.data[i] - 'A') < 26) {
      s.data[i] |= 32;
    }
  }
  return s;
}

typedef struct {
  S8 head, tail;
} S8pair;

static S8pair s8cut(S8 s, U8 c) {
  S8pair r = {0};
  if (s.data) {
    U8 *beg = s.data;
    U8 *end = s.data + s.len;
    U8 *cut = beg;
    for (; cut < end && *cut != c; cut++) {}
    r.head = s8span(beg, cut);
    if (cut < end) {
      r.tail = s8span(cut + 1, end);
    }
  }
  return r;
}

static S8 s8i64(Arena *arena, I64 x) {
  _Bool negative = (x < 0);
  if (negative) { x = -x; }
  char digits[20];
  int i = countof(digits);
  do {
    digits[--i] = (char)(x % 10) + '0';
  } while (x /= 10);
  Size len = countof(digits) - i + negative;
  U8 *beg = newbeg(arena, U8, len);
  U8 *end = beg;
  if (negative) { *end++ = '-'; }
  do { *end++ = digits[i++]; } while (i < countof(digits));
  return (S8){beg, len};
}

#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

__AFL_FUZZ_INIT();

int main() {
  __AFL_INIT();
  enum { HEAP_CAP = 1u << 28, };
  U8 *heap = malloc(HEAP_CAP);
  assert(heap);
  Arena arena[1] = { (Arena){heap, heap + HEAP_CAP}, };

  U8 *buf = __AFL_FUZZ_TESTCASE_BUF;
  while (__AFL_LOOP(10000)) {
    Size len = __AFL_FUZZ_TESTCASE_LEN;
    Arena scratch = *arena;

    S8 full = s8span(buf, buf + len);
    S8 dup = s8dup(&scratch, full);
    dup = s8tolower(dup);
    Size nullbyte = dup.len / 2;
    if (nullbyte > 0) {
      dup.data[nullbyte] = 0;
      S8 cstr = s8cstr(&scratch, (char *)dup.data);
    }

    S8pair cut = {0};
    cut.tail = full;
    while (cut.tail.data) {
      cut = s8cut(cut.head, ' ');
      S8 trimmed = s8trimspace(cut.head);
      s8match(cut.head, trimmed, trimmed.len);
    }
  }
}
