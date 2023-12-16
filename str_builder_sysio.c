typedef __UINT8_TYPE__   u8;
typedef __UINT32_TYPE__  u32;
typedef __INT32_TYPE__   i32;
typedef __INT32_TYPE__   b32;
typedef __INT64_TYPE__   i64;
typedef __UINTPTR_TYPE__ uptr;
typedef __PTRDIFF_TYPE__ size;
typedef __SIZE_TYPE__    usize;

#define sizeof(s)  (size)sizeof(s)
#define alignof(s) (size)_Alignof(s)
#define countof(s) (sizeof(s) / sizeof(*(s)))
#define assert(c)  while((!(c))) __builtin_unreachable()

#define HEAP_CAPACITY (1<<28)

b32  os_write(i32 fd, u8 *buf, size len);
b32  os_read (i32 fd, u8 *buf, size len);
void os_exit (int status);

////////////////////////////////////////////////////////////////////////////////
#define new(...) new_(__VA_ARGS__, new3, new2)(__VA_ARGS__)
#define new_(a,b,c,d,...) d
#define new2(a, t)    (t *) arena_alloc(a, sizeof(t), alignof(t), 1)
#define new3(a, t, n) (t *) arena_alloc(a, sizeof(t), alignof(t), (n))

typedef struct {
  // REVIEW: We only need an *at, *end pointer
  u8 *backing;
  u8 *at;
  size capacity;
} Arena;

Arena arena_init(u8 *backing, size capacity)
{
  Arena result = {};
  result.at = result.backing = backing;
  result.capacity = capacity;
  return result;
}

__attribute__((malloc, alloc_size(2,4), alloc_align(3)))
u8 *arena_alloc(Arena *a, size objsize, size align, size count)
{
  size avail = (a->backing + a->capacity) - a->at;
  size padding = -(uptr)a->at & (align - 1); // TODO: figure this out
  size total   = padding + objsize * count;
  if (avail < total) {
    os_write(2, (u8 *)"Out of Memory", 13);
    os_exit(2);
  }

  u8 *p = a->at + padding;
  a->at += total;

  for (size i = 0; i < objsize * count; i++) {
    p[i] = 0;
  }

  return p;
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
#define S(s)        (Str){ .buf = (u8 *)(s), .len = countof((s)) - 1, }
#define S_PRINT(s)  (int)s.len, s.buf
#define S_FMT       "%.*s"

typedef struct {
  u8 *buf;
  size len;
} Str;

void str_write(Str s)
{
  os_write(1, s.buf, s.len);
}
////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
typedef struct {
  u8 *items;
  int count;
  int capacity;
} Str_Builder;

Str_Builder sb_init(Arena *arena, size capacity)
{
  if (capacity == 0) { capacity = 1024; }

  Str_Builder result = {0};
  result.items = new(arena, u8, capacity);
  result.capacity = capacity;
  return result;
}

void sb_append_u8(Str_Builder *sb, u8 c)
{
  assert(sb->count + 1 < sb->capacity);
  sb->items[sb->count++] = c;
}

void sb_append_str(Str_Builder *sb, Str str)
{
  for (size i = 0; i < str.len; i++) {
    sb_append_u8(sb, str.buf[i]);
  }
}

void sb_append_long(Str_Builder *sb, long x)
{
  unsigned char tmp[64];
  unsigned char *end = tmp + sizeof(tmp);
  unsigned char *beg = end;
  long t = x>0 ? -x : x;
  do {
    *--beg = '0' - t%10;
  } while (t /= 10);
  if (x < 0) {
    *--beg = '-';
  }
  sb_append_str(sb, (Str){.buf = beg, .len = end - beg});
}

////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
int run(Arena *heap)
{
  Str_Builder sb = sb_init(heap, 0);
  sb_append_str(&sb, S("Hello World!"));
  sb_append_u8(&sb, '\n');
  os_write(1, sb.items, sb.count);

  /* u8 buf[1 << 10]; */
  /* int nbytes = os_read(0, buf, 1 << 10); */

  /* sb.count = 0; */
  /* sb_append_str(&sb, S("Got some input: ")); */
  /* sb_append_long(&sb, nbytes); */
  /* sb_append_u8(&sb, '\n'); */
  /* os_write(1, sb.items, sb.count); */

  return 0;
}
////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
// $ cc -nostdlib -fno-builtin str_builder_sysio.c
b32 os_write(i32 fd, u8 *buf, size len)
{
  for (size off = 0; off < len;) {
    size r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(1), "D"(fd), "S"(buf + off), "d"(len - off)
                 : "rcx", "r11", "memory"
                 );
    if (r < 1) {
      return 0;
    }
    off += r;
  }
  return 1;
}

i32 os_read(i32 fd, u8 *buf, size len)
{
  for (size off = 0; off < len;) {
    size r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(0), "D"(fd), "S"(buf + off), "d"(len - off)
                 : "rcx", "r11", "memory"
                 );
    if (r < 0) return 0;
    if (r == 0) return off;
    off += r;
  }
  return len;
}

__attribute__((noreturn))
void os_exit(int status)
{
  asm volatile ("syscall" : : "a"(60), "D"(status));
  assert(0);
}

__attribute__((force_align_arg_pointer))
void _start(void)
{
  static u8 memory[HEAP_CAPACITY] __attribute__((aligned(64)));
  Arena heap = arena_init(memory, HEAP_CAPACITY);
  int r = run(&heap);
  os_exit(r);
}
////////////////////////////////////////////////////////////////////////////////
