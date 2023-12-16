// Platform: x86-64 POSIX
//
// $ cc -nostdlib -fno-builtin rectcut.c
//
// Dead simple UI placement algo.
//

////////////////////////////////////////////////////////////////////////////////
// Types and Utils

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
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// Platform Agnostic Layer

#define HEAP_CAPACITY (1<<28)

b32  os_write(i32 fd, u8 *buf, size len);
b32  os_read (i32 fd, u8 *buf, size len);
void os_exit (int status);
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// Arena Allocator

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
//// Buffered IO
////////////////////////////////////////////////////////////////////////////////
typedef struct  {
  u8 *buf;
  i32 capacity;
  i32 len;
  int fd;
  _Bool error;
} Write_Buffer;

Write_Buffer write_buffer(u8 *buf, i32 capacity);
Write_Buffer fd_buffer(int fd, u8 *buf, i32 capacity);
void append(Write_Buffer *b, unsigned char *src, int len);
void flush();

Write_Buffer write_buffer(u8 *buf, i32 capacity)
{
  Write_Buffer result = {0};
  result.buf = buf;
  result.capacity = capacity;
  result.fd = -1;
  return result;
}

Write_Buffer fd_buffer(int fd, u8 *buf, i32 capacity)
{
  Write_Buffer result = {0};
  result.buf = buf;
  result.capacity = capacity;
  result.fd = fd;
  return result;
}

void append(Write_Buffer *b, unsigned char *src, int len)
{
  unsigned char *end = src + len;
  while (!b->error && src<end) {
    int left = end - src;
    int avail = b->capacity - b->len;
    int amount = avail<left ? avail : left;

    for (int i = 0; i < amount; i++) {
      b->buf[b->len+i] = src[i];
    }
    b->len += amount;
    src += amount;

    if (amount < left) {
      flush(b);
    }
  }
}

#define append_lit(b, s) append(b, (unsigned char*)s, sizeof(s) - 1)

void append_byte(Write_Buffer *b, unsigned char c)
{
  append(b, &c, 1);
}

void append_long(Write_Buffer *b, long x)
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
  append(b, beg, end-beg);
}

void flush(Write_Buffer *b) {
  b->error |= b->fd < 0;
  if (!b->error && b->len) {
    b->error |= !os_write(b->fd, b->buf, b->len);
    b->len = 0;
  }
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//// String slice
////////////////////////////////////////////////////////////////////////////////
#define S(s)        (Str){ .buf = (u8 *)(s), .len = countof((s)) - 1, }
#define S_PRINT(s)  (int)s.len, s.buf
#define S_FMT       "%.*s"
#define null_str    (Str){0}

typedef struct {
  u8 *buf;
  size len;
} Str;

void append_str(Write_Buffer *b, Str s)
{
  append(b, s.buf, s.len);
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//// Rectcut UI layout
////////////////////////////////////////////////////////////////////////////////
#define height 400
#define width  400

void append_svg_attr_str(Write_Buffer *b, Str name, Str val)
{
  append_str(b, name);
  append_str(b, S("=\""));
  append_str(b, val);
  append_str(b, S("\""));
}

void append_svg_attr_ints(Write_Buffer *b, Str name, i32 *xs, size count)
{
  append_str(b, name);
  append_str(b, S("=\""));
  for (size i = 0; i < count; i++) {
    if (i > 0) append_byte(b, ' ');
    append_long(b, xs[i]);
  }
  append_str(b, S("\" "));
}

typedef struct {
  i32 x, y, w, h;
} Rectangle;

Rectangle rectcut_top(Rectangle *r, i32 amount)
{
  Rectangle result = {0};
  result.x = r->x;
  result.y = r->y;
  result.w = r->w;
  result.h = amount;

  r->y += amount;
  r->h -= amount;

  return result;
}

Rectangle rectcut_left(Rectangle *r, i32 amount)
{
  Rectangle result = {0};
  result.x = r->x;
  result.y = r->y;
  result.w = amount;
  result.h = r->h;

  r->x += amount;
  r->w -= amount;

  return result;
}

Rectangle rectcut_right(Rectangle *r, i32 amount)
{
  Rectangle result = {0};
  result.x = r->x + r->w - amount;
  result.y = r->y;
  result.w = amount;
  result.h = r->h;

  r->w -= amount;

  return result;
}

void append_svg_rectcut(Write_Buffer *b, Rectangle r, Str color, Str name)
{
  append_lit(b, "<rect style=\"fill:none;stroke:");
  if (color.buf) append_str(b, color);
  else           append_lit(b, "red");
  append_lit(b, ";stroke-width:8\" ");

  append_svg_attr_ints(b, S("x"), &r.x, 1);
  append_svg_attr_ints(b, S("y"), &r.y, 1);
  append_svg_attr_ints(b, S("width"), &r.w, 1);
  append_svg_attr_ints(b, S("height"), &r.h, 1);
  append_lit(b, "/>\n");

  int text_x = r.x + 5;
  int text_y = r.y + 20;
  append_lit(b, "<text ");
  append_svg_attr_ints(b, S("x"), &text_x, 1);
  append_svg_attr_ints(b, S("y"), &text_y, 1);
  append_lit(b, ">");
  append_str(b, name);
  append_lit(b, "</text>\n");
}

int run(Arena *heap)
{
  size stdout_capacity = 2024;
  Write_Buffer stdout[1] = { fd_buffer(1, new(heap, u8, stdout_capacity), stdout_capacity) };

  i32 view_box[4] = {0, 0, width, height};
  append_lit(stdout, "<svg xmlns=\"http://www.w3.org/2000/svg\" ");
  append_svg_attr_ints(stdout, S("viewBox"), view_box, countof(view_box));
  append_lit(stdout, ">\n");
  {
    Rectangle rect = (Rectangle){ 0, 0, width, height };
    append_svg_rectcut(stdout, rect, S("#000000"), S(""));

    Rectangle top_panel = rectcut_top(&rect, 30);
    Rectangle file  = rectcut_left(&top_panel, 50);
    Rectangle edit  = rectcut_left(&top_panel, 50);
    Rectangle close = rectcut_right(&top_panel, 50);

    append_svg_rectcut(stdout, top_panel, S("#DDDDDD"), S("top panel"));
    append_svg_rectcut(stdout, rect,      S("#DDDDDD"), S("body"));

    append_svg_rectcut(stdout, file,  null_str, S("file"));
    append_svg_rectcut(stdout, edit,  null_str, S("edit"));
    append_svg_rectcut(stdout, close, null_str, S("close"));
  }
  append_lit(stdout, "</svg>\n");

  flush(stdout);
  return 0;
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//// SYSCALL
////////////////////////////////////////////////////////////////////////////////
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
  os_exit(run(&heap));
}
////////////////////////////////////////////////////////////////////////////////
