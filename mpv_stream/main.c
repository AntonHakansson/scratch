#if 0
set -e
cc main.c -DHOST -o main -Wall -Wextra -Wno-unused-function -ggdb3 -fsanitize=undefined -lm -ldl
cc main.c -shared -fPIC -o newlibplug.so -Wall -Wextra -Wno-unused-function -ggdb3 -fsanitize=undefined -lm
mv newlibplug.so libplug.so
exit 0
#endif

#include <stdint.h>
#include <stddef.h> // size_t
typedef char      HK_Byte; // raw memory aliasing type
typedef uint8_t   HK_U8;
typedef uint32_t  HK_U32;
typedef int32_t   HK_I32;
typedef uint64_t  HK_U64;
typedef int64_t   HK_I64;
typedef ptrdiff_t HK_Size;
typedef size_t    HK_USize;

typedef HK_I32 HK_B32;

#if !defined(HK_OVERRIDE_MEMORY)
# include <stdlib.h>
# include <string.h> // memset
# include <stdio.h> // stderr
# define hk_memset(dst, value, n_bytes) memset((dst), (value), (n_bytes))
# define hk_memcpy(dst, src, n_bytes)   memcpy((dst), (src), (n_bytes))
# define hk_oom() do {                                                      \
    fprintf(stderr, "[FATAL]: Out of Memory: %s:%d\n", __FILE__, __LINE__); \
    exit(2);                                                                \
  } while(0)
# define hk_malloc(n_bytes) malloc((n_bytes))
# define hk_free(ptr) free((ptr))
# define hk_realloc(ptr, n_bytes) realloc((ptr), (n_bytes))
#endif // HK_OVERRIDE_MEMORY

#if !defined(HK_OVERRIDE_IO)
# include <unistd.h>
# include <errno.h>
# define hk_write(fd, buf, n_bytes) hk_xwrite((fd), (buf), (n_bytes))
static int hk_xwrite(int fd, const void *p, int64_t n_bytes)
{
  const char *buf = p;
  while (n_bytes > 0) {
    int64_t written = 0;
    do {
      written = write(fd, buf, n_bytes);
    } while(written < 0 && errno == EINTR);
    if (written < 0) {
      return -1;
    }
    buf += written;
    n_bytes -= written;
  }
  return 0;
}
#endif //  HK_OVERRIDE_IO

#define hk_size_of(s) (HK_Size)sizeof(s)
#define hk_align_of(s) (HK_Size)_Alignof(s)
#define hk_assert(c)  while((!(c))) __builtin_unreachable()
#define hk_static_assert(c, label) char hk_static_assert_##label[(c) ? (1) : (-1)]

#define hk_count_of(s) (hk_size_of(s) / hk_size_of(*(s)))
#define hk_return_defer(r)  do { result = (r); goto defer; } while(0)


////////////////////////////////////////////////////////////////////////////////
//- Global Thread Context

#define HK_THREAD_SCRATCH_ARENA_COUNT (2)
#define HK_THREAD_SCRATCH_ARENA_CAPACITY (32 * 1024 * 1024)

#define hk_thread_malloc(n_bytes)    hk_thread_ctx.allocator(hk_thread_ctx.allocator_ctx, 0,     (n_bytes), 0)
#define hk_thread_free(ptr, n_bytes) hk_thread_ctx.allocator(hk_thread_ctx.allocator_ctx, (ptr), (n_bytes), 0)
#define hk_thread_realloc(ptr, n_bytes, old_bytes) hk_thread_ctx.allocator(hk_thread_ctx.allocator_ctx, (ptr), (n_bytes), (old_bytes))

typedef void *(*HK_Allocator_Func)(void *ctx, void *ptr, HK_Size n_bytes, HK_Size old_n_bytes);
static void *hk_default_allocator(void *ctx, void *ptr, HK_Size n_bytes, HK_Size old_n_bytes);

typedef struct HK_Thread_Context {
  HK_Allocator_Func allocator;
  void *allocator_ctx;
  struct HK_Arena *scratch_pool[HK_THREAD_SCRATCH_ARENA_COUNT];
} HK_Thread_Context;

static __thread HK_Thread_Context hk_thread_ctx = {
  .allocator = hk_default_allocator,
};

static void *hk_default_allocator(void *ctx, void *ptr, HK_Size n_bytes, HK_Size old_n_bytes)
{
  (void)ctx;
  if (ptr == 0) {
    return hk_malloc((HK_USize)n_bytes);
  }
  else if (old_n_bytes == 0) {
    hk_free(ptr);
    return 0;
  }
  else {
    return hk_realloc(ptr, (HK_USize)n_bytes);
  }
}


////////////////////////////////////////////////////////////////////////////////
//- Arena Allocator

#define hk_new(a, t, n) (t *) hk_arena_alloc(a, hk_size_of(t), hk_align_of(t), (n))

typedef struct HK_Arena {
  HK_Size capacity;
  HK_Size at;
  HK_Byte *backing;
} HK_Arena;

typedef struct HK_Arena_Scope {
  HK_Arena *arena;
  HK_Size at_rewind;
} HK_Arena_Scope;

static HK_Arena *hk_arena_make(HK_Size capacity)
{
  HK_Arena *a = hk_thread_malloc(capacity);
  HK_Size backing_offset = hk_size_of(*a);
  hk_assert(capacity > backing_offset);
  a->backing = (HK_Byte*)a + backing_offset;
  a->capacity = capacity - backing_offset;
  a->at = 0;
  return a;
}

__attribute__((unused))
static void hk_arena_free(HK_Arena *a)
{
  HK_Size backing_offset = hk_size_of(*a);
  hk_thread_free(a, backing_offset + a->capacity);
}

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static HK_Byte *hk_arena_alloc(HK_Arena *a, HK_Size objsize, HK_Size align, HK_Size count)
{
  HK_Size avail = a->capacity - a->at;
  HK_Size padding = -(uintptr_t)(a->backing + a->at) & (align - 1);
  HK_Size total   = padding + objsize * count;
  if (avail < total) { hk_oom(); }

  HK_Byte *p = (a->backing + a->at) + padding;
  a->at += total;

  memset(p, 0, objsize * count);

  return p;
}

static void hk_arena_rewind(HK_Arena *a, HK_Size point)
{
  a->at = point;
}

static HK_Arena_Scope hk_arena_scope_begin(HK_Arena *a)
{
  HK_Arena_Scope result = {0};
  result.arena = a;
  result.at_rewind = a->at;
  return result;
}

static void hk_arena_scope_end(HK_Arena_Scope scope)
{
  hk_arena_rewind(scope.arena, scope.at_rewind);
}

static HK_Arena *hk_arena_scratch_(HK_Arena **conflicts, HK_Size conflicts_count)
{
  hk_assert(conflicts_count < HK_THREAD_SCRATCH_ARENA_COUNT);

  HK_Arena **scratch_pool = hk_thread_ctx.scratch_pool;

  if (scratch_pool[0] == 0) {
    for (HK_Size i = 0; i < HK_THREAD_SCRATCH_ARENA_COUNT; i++) {
      scratch_pool[i] = hk_arena_make(HK_THREAD_SCRATCH_ARENA_CAPACITY);
    }
    return scratch_pool[0];
  }

  for (HK_Size i = 0; i < HK_THREAD_SCRATCH_ARENA_COUNT; i++) {
    for (HK_Size j = 0; j < conflicts_count; j++) {
      if (scratch_pool[i] == conflicts[j]) {
        break;
      }
      else {
        return scratch_pool[i];
      }
    }
  }

  if (conflicts_count == 0) {
    return scratch_pool[0];
  }
  hk_assert(0 && "unreachable if logic above prevented conflicting arenas.");
  return 0;
}

static HK_Arena_Scope hk_arena_scratch(HK_Arena **conflicts, HK_Size conflicts_count)
{
  HK_Arena *a = hk_arena_scratch_(conflicts, conflicts_count);
  HK_Arena_Scope result = {0};
  if (a) {
    result = hk_arena_scope_begin(a);
  }
  return result;
}


////////////////////////////////////////////////////////////////////////////////
//- String slice

#define S(s)        (HK_Str){ .buf = (HK_U8 *)(s), .count = hk_count_of((s)) - 1, }
#define S_FMT(s)    (HK_I32)(s).count, (s).buf  /* printf("%.*s", S_FMT(str)) */

typedef struct {
  HK_U8  *buf;
  HK_Size count;
} HK_Str;

static HK_Str hk_str_from_cstr(char *cstr)
{
  HK_Str result = {0};
  result.buf = (HK_U8 *)cstr;
  while(*(cstr++)) { result.count += 1; }
  return result;
}


////////////////////////////////////////////////////////////////////////////////
//- Write Buffer w/ optional flushing to file

typedef struct HK_Write_Buffer {
  HK_U8 *buf;
  HK_Size capacity;
  HK_Size count;
  HK_I32 fd; // 0 = memory buffer
  HK_I32 error;
} HK_Write_Buffer;

#define hk_append(b, ...)                              \
 hk_append_strs((b),                                   \
                ((HK_Str[]){__VA_ARGS__}),             \
                hk_count_of(((HK_Str[]){__VA_ARGS__})))

static HK_Write_Buffer hk_write_buffer_make(HK_Size capacity, HK_I32 fd, HK_Arena *arena)
{
  HK_Write_Buffer result = {0};
  result.buf = hk_new(arena, HK_U8, capacity);
  result.capacity = capacity;
  result.fd = fd;
  return result;
}

static void hk_write_buffer_flush(HK_Write_Buffer *b)
{
  if (b->fd <= 0) { b->error = 1; }
  if (!b->error && b->count > 0) {
    b->error |= hk_write(b->fd, b->buf, b->count);
    b->count = 0;
  }
}

static void hk_append_bytes(HK_Write_Buffer *b, HK_U8 *src, HK_Size n_bytes)
{
  HK_U8 *end = src + n_bytes;
  while (!b->error && src < end) {
    HK_Size left = (end - src);
    HK_Size avail = b->capacity - b->count;
    HK_Size amount = (avail < left) ? avail : left;
    hk_memcpy(b->buf + b->count, src, amount);
    b->count += amount;
    src += amount;
    if (amount < left) { hk_write_buffer_flush(b); }
  }
}

static void hk_append_byte(HK_Write_Buffer *b, HK_U8 byte)
{
  hk_append_bytes(b, &byte, 1);
}

static void hk_append_strs(HK_Write_Buffer *b, HK_Str *strs, HK_Size strs_count)
{
  for (HK_Size i = 0; i < strs_count; i++) {
    hk_append_bytes(b, strs[i].buf, strs[i].count);
  }
}

static void hk_append_long(HK_Write_Buffer *b, long x)
{
  unsigned char tmp[64];
  unsigned char *end = tmp + hk_size_of(tmp);
  unsigned char *beg = end;
  long t = x>0 ? -x : x;
  do {
    *--beg = '0' - (unsigned char)(t%10);
  } while (t /= 10);
  if (x < 0) {
    *--beg = '-';
  }
  hk_append_bytes(b, beg, end-beg);
}


typedef struct HK_Write_Buffer_Cursor {
  HK_Write_Buffer *b;
  HK_Size at;
} HK_Write_Buffer_Cursor;


__attribute__((unused))
static HK_Write_Buffer_Cursor hk_write_buffer_cursor(HK_Write_Buffer *b)
{
  HK_Write_Buffer_Cursor result = {0};
  result.b = b;
  result.at = b->count;
  return result;
}

__attribute__((unused))
static HK_Str hk_write_buffer_cursor_advance(HK_Write_Buffer_Cursor *cursor)
{
  HK_Str result = {0};
  hk_assert(cursor->b->count >= cursor->at && "Buffer flushed - this state should not be legal");
  if (cursor->b->count >= cursor->at) {
    result.buf   = cursor->b->buf + cursor->at;
    result.count = cursor->b->count - cursor->at;
    cursor->at   = cursor->b->count;
  }
  return result;
}


////////////////////////////////////////////////////////////////////////////////
//- Linked Lists
/*
 * struct T { T *child; };
 * #define t_push_child(t, node) hk_stack_push_n((t)->child, (node), child)
 *
 */

#define hk_stack_push_n(first, node, next_member) ((node)->next_member = (first), (first) = (node))
#define hk_stack_pop_n(first, next_member)        (((first) == 0) ? 0 : (first) = (first)->next_member)
#define hk_queue_push_n(first, last, node, next_member) (((first) == 0) ? \
  (first = (last) = node) : \
  ((last)->next_member = (node), (last) = (node), (node)->next_member = 0))
#define hk_queue_pop_n(first, last, next_member) ((first) == (last) ? \
  ((first) = (last) = 0) : ((first) = (first)->next_member))


////////////////////////////////////////////////////////////////////////////////
//- PPM

#define PPM_COMPONENTS 3
typedef struct PPM {
  HK_U8 *pixels;
  HK_Size width;
  HK_Size height;
} PPM;

#define PPM_AT(ppm, x, y) ((ppm)->pixels + ((y) * (ppm)->width * PPM_COMPONENTS) + ((x) * PPM_COMPONENTS))

static PPM ppm_init(HK_Arena *a, HK_Size width, HK_Size height)
{
  PPM result = {0};
  result.pixels = hk_new(a, HK_U8, width * height * PPM_COMPONENTS);
  result.width = width;
  result.height = height;
  return result;
}

static void ppm_write(PPM ppm, HK_Write_Buffer *b)
{
  hk_append(b, S("P6\n"));
  hk_append_long(b, ppm.width);
  hk_append_byte(b, ' ');
  hk_append_long(b, ppm.height);
  hk_append_byte(b, '\n');
  hk_append_long(b, 255);
  hk_append_byte(b, '\n');
  hk_append_bytes(b, ppm.pixels, ppm.width * ppm.height * PPM_COMPONENTS);
  hk_write_buffer_flush(b);
}

static void ppm_set(PPM *ppm, HK_I32 x, HK_I32 y, HK_U32 color)
{
  if (!(x >= 0 && y >= 0 && x < ppm->width && y < ppm->height)) return;
  HK_U8 *p = PPM_AT(ppm, x, y);
  *(p + 0) = color >> 16;
  *(p + 1) = color >> 8;
  *(p + 2) = color >> 0;
}

static HK_U32 ppm_get(PPM *ppm, HK_I32 x, HK_I32 y)
{
  if (!(x >= 0 && y >= 0 && x < ppm->width && y < ppm->height)) return 0;
  HK_U8 *p = PPM_AT(ppm, x, y);
  HK_U32 result = 0;
  result = (*(p++)) << 16;
  result = (*(p++)) << 8;
  result = (*(p++)) << 0;
  return result;
}

static void ppm_clear(PPM *ppm, HK_U32 col)
{
  for (int y = 0; y < ppm->height; y++) {
    for (int x = 0; x < ppm->width; x++) {
      ppm_set(ppm, x, y, col);
    }
  }
}

#define rgb(...) rgb_join((RGB){ __VA_ARGS__ })

typedef struct RGB {
  HK_U8 r, g, b;
} RGB;

static HK_U32 rgb_join(RGB col)
{
  HK_U32 result = 0;
  result  = (col.r) << 16;
  result |= (col.g) << 8;
  result |= (col.b) << 0;
  return result;
}

static RGB rgb_split(HK_U32 col)
{
  RGB result = {0};
  result.r = (col >> 16) & 0xFF;
  result.g = (col >>  8) & 0xFF;
  result.b = (col >>  0) & 0xFF;
  return result;
}


void ppm_aabb(PPM *ppm, HK_I32 x, HK_I32 y, HK_I32 w, HK_I32 h, HK_U32 col)
{
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      ppm_set(ppm, x + dx, y + dy, col);
    }
  }
}

#include "font.h"
#include "math.h"
static float font_value(int c, int x, int y)
{
  if (c < 32 || c > 127) { return 0.0f; }
  int cx = c % 16;
  int cy = (c - 32) / 16;
  int v = font[(cy * FONT_H + y) * FONT_W * 16 + (cx * FONT_W) + x];
  return sqrtf(v / 255.0f);
}

static void ppm_char(PPM *ppm, HK_I32 c, HK_I32 x, HK_I32 y, HK_U32 fgc)
{
  RGB fg = rgb_split(fgc);
  for (int dy = 0; dy < FONT_H; dy++) {
    for (int dx = 0; dx < FONT_W; dx++) {
      float a = font_value(c, dx, dy);
      if (a > 0.0f) {
        unsigned long bgc = ppm_get(ppm, x + dx, y + dy);
        RGB bg = rgb_split(bgc);
        RGB out = {
          .r = a * fg.r + (1 - a) * bg.r,
          .g = a * fg.g + (1 - a) * bg.g,
          .b = a * fg.b + (1 - a) * bg.b,
        };
        ppm_set(ppm, x + dx, y + dy, rgb_join(out));
      }
    }
  }
}

static void ppm_str(PPM *ppm, HK_Str str, HK_I32 x, HK_I32 y, HK_U32 fgc)
{
  for (HK_Size i = 0; i < str.count; i++) {
    ppm_char(ppm, str.buf[i], x + i * FONT_W, y, fgc);
  }
}


////////////////////////////////////////////////////////////////////////////////
//- Program

enum {
  WIDTH  = 600,
  HEIGHT = 400,

  FPS = 60,
};

#define PLUG_UPDATE(name) HK_Size name(HK_Write_Buffer *stderr, HK_Arena *perm, PPM *ppm, float dt)
typedef PLUG_UPDATE((plug_update_func));

#ifndef HOST
static HK_I32 round_i32(float f) {
  return (HK_I32)(f + 0.5);
}

typedef struct State {
  float elapsed;
} State;

static State *state = 0;
PLUG_UPDATE(update)
{
  if (perm->at == 0) { state = hk_new(perm, State, 1); }
  if (!state)        { state = (State *)perm->backing; }

  float oscillating01 = (cosf(state->elapsed) + 1.0f) * 0.5f;
  float x = (oscillating01 * WIDTH * 0.5f);
  ppm_str(ppm, S("Heeloope!! this"), round_i32(x), 0, rgb(.r = 255, .g = 122));
  ppm_aabb(ppm, round_i32((1.0 - oscillating01) * WIDTH * 0.5f), 50, 50, 50, rgb(.b=255));
  ppm_aabb(ppm, round_i32((1.0 - oscillating01) * WIDTH * 0.5f), 50 * 3, 50, 50, rgb(.b=255));

  state->elapsed += dt;

  return 0;
}

#else

typedef struct Plug_API {
  void *dlhandle;
  HK_I64 last_modified;
  plug_update_func *update;
} Plug_API;

typedef struct Plug {
  HK_Arena *arena;
  Plug_API api;
} Plug;

PLUG_UPDATE(update_stub) { return 0; }


#include <dlfcn.h>
#include <sys/stat.h>

HK_B32 plug_reload(Plug_API api[static 1], char *plug_path, HK_Write_Buffer stderr[static 1])
{
  HK_B32 result = 0;

  struct stat statbuf = {0};
  if (stat(plug_path, &statbuf) < 0) {
    hk_append(stderr, S("[ERROR]: Could not stat file "),
              hk_str_from_cstr(plug_path),
              S(": "),
              hk_str_from_cstr(strerror(errno)));
  }

  HK_I64 last_modified = statbuf.st_mtime;
  if (last_modified > api->last_modified) {
    if (api->dlhandle) {
      hk_append(stderr, S("[INFO]: Reloading plug\n"));
      if (dlclose(api->dlhandle)) {
        hk_append(stderr, S("[WARN]: dlclose failed: "), hk_str_from_cstr(dlerror()));
      }
    }
    memset(api, 0, sizeof(*api));
    api->last_modified = last_modified;

    void *plug_lib = dlopen(plug_path, RTLD_NOW);
    if (plug_lib) {
      void *update_func = dlsym(plug_lib, "update");
      if (update_func) {
        api->dlhandle = plug_lib;
        api->update = update_func;
        result = 1;
      }
      else {
        hk_append(stderr, S("[ERROR]: update symbol not found: "), hk_str_from_cstr(dlerror()));
      }
    }
    else {
      hk_append(stderr, hk_str_from_cstr(dlerror()));
    }
  }

  return result;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  HK_Arena *perm = hk_arena_make(32 * 1024 * 1024);
  HK_Write_Buffer stdout[1] = { hk_write_buffer_make(8 * 1024, 1, perm) };
  HK_Write_Buffer stderr[1] = { hk_write_buffer_make(8 * 1024, 2, perm) };

  PPM ppm[1] = { ppm_init(perm, WIDTH, HEIGHT) };

  Plug plug = {0};
  plug.arena = hk_arena_make(32 * 1024 * 1024);

  float dt = 1.f / FPS;
  for (HK_USize frame = 0; 1; frame++) {
    if (!plug_reload(&plug.api, "./libplug.so", stderr)) {
      hk_write_buffer_flush(stderr);
    }
    plug.api.update(stdout, plug.arena, ppm, dt);
    ppm_write(*ppm, stdout);
    ppm_clear(ppm, 0);
    hk_write_buffer_flush(stdout);
    hk_write_buffer_flush(stderr);
  }

  hk_arena_free(perm);
  return 0;
}
#endif

/* Local Variables: */
/* compile-command: "sh main.c" */
/* End: */
