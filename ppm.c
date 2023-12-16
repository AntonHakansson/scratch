////////////////////////////////////////////////////////////////////////////////
//- Types and Utils

typedef __UINT8_TYPE__   u8;
typedef __UINT32_TYPE__  u32;
typedef __INT32_TYPE__   i32;
typedef __INT32_TYPE__   b32;
typedef __INT64_TYPE__   i64;
typedef __UINT64_TYPE__  u64;
typedef __UINTPTR_TYPE__ uptr;
typedef __PTRDIFF_TYPE__ size;
typedef __SIZE_TYPE__    usize;

#define sizeof(s)  (size)sizeof(s)
#define alignof(s) (size)__alignof(s)
#define countof(s) (sizeof(s) / sizeof(*(s)))
#define assert(c)  while((!(c))) __builtin_unreachable()


////////////////////////////////////////////////////////////////////////////////
//- Arena Allocator

#define new(a, t, n) (t *) arena_alloc(a, sizeof(t), alignof(t), (n))

typedef struct {
  u8 *backing, *at;
  size capacity;
} Arena;

Arena arena_init(u8 *backing, size capacity)
{
  Arena result = {0};
  result.at = result.backing = backing;
  result.capacity = capacity;
  return result;
}

__attribute__((malloc, alloc_size(2,4), alloc_align(3)))
u8 *arena_alloc(Arena *a, size objsize, size align, size count)
{
  size avail = (a->backing + a->capacity) - a->at;
  size padding = -(uptr)a->at & (align - 1);
  size total   = padding + objsize * count;
  if (avail < total) {
    // TODO: out of memory policy
    assert(0);
  }

  u8 *p = a->at + padding;
  a->at += total;

  for (size i = 0; i < objsize * count; i++) {
    p[i] = 0;
  }

  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- Dynamic Arrays w/ Arena Allocator
//
// When an array needs to grow we allocate a new larger chunk from the arena and copy over
// the old array. The old array is preserved on the arena. Be catious about holding
// pointers as they refer to old data after relocation.
//
// The design is composable such that any struct that has items, capacity
// and len members can be turned into a dynamic array.
//

#define da_init(a, t, cap) ({                                           \
      t *s = new(a, t, 1);                                              \
      s->capacity = cap;                                                \
      s->items = (typeof(*s->items) *)                                  \
        arena_alloc((a), sizeof(*(s)->items), _Alignof(*(s)->items), cap); \
      s;                                                                \
  })

// Allocates on demand
#define da_push(a, s) ({                                                \
      typeof(s) _s = s;                                                 \
      if (_s->len >= _s->capacity) {                                    \
        da_grow((a), (void **)&_s->items, &_s->capacity, &_s->len,      \
                sizeof(*_s->items), _Alignof(*_s->items));              \
      }                                                                 \
      _s->items + _s->len++;                                            \
    })

// Assumes new item fits in capacity
#define da_push_unsafe(s) ({                    \
      assert((s)->len < (s)->capacity);         \
      (s)->items + (s)->len++;                  \
    })

void da_grow(Arena *arena, void **__restrict items, size *__restrict capacity, size *__restrict len, size item_size, size align)
{
  assert(*items != 0 && *capacity > 0);
  u8 *items_end = (((u8*)(*items)) + (item_size * (*len)));
  if (arena->at == items_end) {
    // Extend in place, no allocation occured between da_grow calls
    arena_alloc(arena, item_size, align, (*capacity));
    *capacity *= 2;
  }
  else {
    // Relocate array
    u8 *p = arena_alloc(arena, item_size, align, (*capacity) * 2);
#define DA_MEMORY_COPY(dst, src, bytes) for (int i = 0; i < bytes; i++) { ((char *)dst)[i] = ((char *)src)[i]; }
    DA_MEMORY_COPY(p, *items, (*len) * item_size);
#undef DA_MEMORY_COPY
    *items = (void *)p;
    *capacity *= 2;
  }
}

////////////////////////////////////////////////////////////////////////////////
//- PPM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PPM_COMPONENTS 3
typedef struct PPM {
  u8 *pixels;
  size width;
  size height;
} PPM;

#define PPM_AT(ppm, x, y) ((ppm)->pixels + ((y) * (ppm)->width * PPM_COMPONENTS) + ((x) * PPM_COMPONENTS))

PPM ppm_init(Arena *a, size width, size height)
{
  PPM result = {0};
  result.pixels = new(a, u8, width * height * PPM_COMPONENTS);
  result.width = width;
  result.height = height;
  return result;
}

void ppm_write(PPM ppm, FILE *f)
{
  fprintf(f, "P6\n%ld %ld\n255\n", ppm.width, ppm.height);
  fwrite(ppm.pixels, ppm.width * PPM_COMPONENTS, ppm.height, f);
  flush(f);
}

void ppm_set(PPM *ppm, i32 x, i32 y, u32 color)
{
  if (!(x >= 0 && y >= 0 && x < ppm->width && y < ppm->height)) return;
  u8 *p = PPM_AT(ppm, x, y);
  *(p + 0) = color >> 16;
  *(p + 1) = color >> 8;
  *(p + 2) = color >> 0;
}

u32 ppm_get(PPM *ppm, i32 x, i32 y)
{
  if (!(x >= 0 && y >= 0 && x < ppm->width && y < ppm->height)) return 0;
  u32 *p = (u32*)PPM_AT(ppm, x, y);
  return *p;
}

void ppm_clear(PPM *ppm, u32 col)
{
  for (int y = 0; y < ppm->height; y++) {
    for (int x = 0; x < ppm->width; x++) {
      ppm_set(ppm, x, y, col);
    }
  }
}

void ppm_aabb(PPM *ppm, i32 x, i32 y, i32 w, i32 h, u32 col)
{
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      ppm_set(ppm, x + dx, y + dy, col);
    }
  }
}

#include <math.h>

float font_value(int c, int x, int y)
{
  #include "font.h"
  if (c < 32 || c > 127) {
    return 0.0f;
  }
  int cx = c % 16;
  int cy = (c - 32) / 16;
  int v = font[(cy * FONT_H + y) * FONT_W * 16 + (cx * FONT_W) + x];
  return sqrtf(v / 255.0f);
}

void ppm_char(PPM *ppm, i32 c, i32 x, i32 y, u32 fgc)
{
  /* float fr, fg, fb; */
  /* rgb_split(fgc, &fr, &fg, &fb); */
  for (int dy = 0; dy < FONT_H; dy++) {
    for (int dx = 0; dx < FONT_W; dx++) {
      float a = font_value(c, dx, dy);
      if (a > 0.0f) {
        /* unsigned long bgc = ppm_get(buf, x + dx, y + dy); */
        /* float br, bg, bb; */
        /* rgb_split(bgc, &br, &bg, &bb); */

        /* float r = a * fr + (1 - a) * br; */
        /* float g = a * fg + (1 - a) * bg; */
        /* float b = a * fb + (1 - a) * bb; */
        /* ppm_set(buf, x + dx, y + dy, rgb_join(r, g, b)); */
        ppm_set(ppm, x + dx, y + dy, fgc);
      }
    }
  }
}

typedef struct PPM_rgb{
  u8 r, g, b;
} PPM_rgb;

u32 join_rgb(PPM_rgb col)
{
  u32 result = 0;
  result  = (col.r) << 16;
  result |= (col.g) << 8;
  result |= (col.b) << 0;
  return result;
}

PPM_rgb split_rgb(u32 col)
{
  PPM_rgb result = {0};
  result.r = col >> 16;
  result.g = col >> 8;
  result.b = col >> 0;
  return result;
}

u32 hue(u32 v, u32 N)
{
  unsigned long h = v / (N / 6);
  unsigned long f = v % (N / 6);
  unsigned long t = 0xff * f / (N / 6);
  unsigned long q = 0xff - t;
  switch (h) {
  case 0:
    return 0xff0000UL | (t << 8);
  case 1:
    return (q << 16) | 0x00ff00UL;
  case 2:
    return 0x00ff00UL | t;
  case 3:
    return (q << 8) | 0x0000ffUL;
  case 4:
    return (t << 16) | 0x0000ffUL;
  case 5:
    return 0xff0000UL | q;
  }
  assert(0 && "unreachable");
}

////////////////////////////////////////////////////////////////////////////////
//- Program

typedef struct I32s {
  i32 *items;
  size capacity;
  size len;
} I32s;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct I32s_History {
  I32s *items;
  size capacity;
  size len;
} I32s_History;

typedef struct Anim {
  i32 frame;
  PPM ppm;
  I32s_History *hist;
  b32 will_fragment;

  i32 camera_y;
  i32 camera_y_target;
} Anim;

void frame(Anim *a)
{
  PPM *ppm = &a->ppm;
  I32s_History *hist = a->hist;

  u32 color_cap         = 0x444444;
  u32 color_locked_cap  = 0xBB0000;

  ppm_clear(ppm, 0);

  i32 grid = 20;

  i32 y_start[1024];
  y_start[0] = 0;
  for (size rev = 1; rev < hist->len; rev++) {
    I32s *prev_ints = &hist->items[rev - 1];
    i32 prev_y_start = y_start[rev - 1];
    i32 prev_height = (prev_ints->capacity * grid / ppm->width) * grid;
    y_start[rev] = prev_y_start + prev_height + grid * 2;
  }

  i32 cur_height = (hist->items[hist->len - 1].len * grid) / ppm->width * grid;
  a->camera_y_target = y_start[hist->len - 1] + cur_height - ppm->height/2;
  a->camera_y += (a->camera_y_target - a->camera_y) * .1f;

  for (size rev = hist->len - 1; rev >= 0; rev--) {
    I32s *ints = &hist->items[rev];

    i32 ystart = y_start[rev];

    i32 y_end   = ystart + (ints->capacity * grid / ppm->width) * grid;
    if (y_end < -100) break;

    for (size i = 0; i < ints->capacity; i++) {
      u32 color = 0;
      if      (i < ints->len) color = hue((u32)ints->items[i] % 360, 360);
      else if (a->will_fragment && (rev == hist->len - 1)) color = color_locked_cap;
      else color = color_cap;

      i32 x = (i * grid) % ppm->width;
      i32 y = ystart + ((i * grid) / ppm->width) * grid;
      ppm_aabb(ppm, x, y - a->camera_y, grid, grid, color);
    }
  }

  // TODO: render some messege here maybe?
  /* ppm_char(ppm, 'a', 0, 400, 0xFFFFFF); */

  ppm_write(*ppm, stdout);
  if (ferror(stdout)) {
    fputs("ppm: error writing video frame\n", stderr);
    exit(1);
  }

  a->frame++;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  size heap_size = 8 * 1024 * 1024;
  u8  *heap      = malloc(heap_size);

  Arena arena[1] = { arena_init(heap, heap_size) };

  Anim *anim = new(arena, Anim, 1);
  anim->ppm = ppm_init(arena, 400, 800);
  anim->hist = da_init(arena, I32s_History, 1024);

  I32s *ints = da_init(arena, I32s, 2);
  *da_push_unsafe(ints) = 6969;
  *da_push_unsafe(ints) = 420;

  *da_push(arena, anim->hist) = *ints;

  size insertions = 1024 * 2;
  for (size i = 0, h = 0x100; i < insertions; i += 1, h *= 1111111111111111111u) {
    I32s prev_ints = *ints;
    *da_push(arena, ints) = abs((int)h) % 1000;
    b32 did_fragment = (ints->items != prev_ints.items);
    if (did_fragment) {
      *da_push(arena, anim->hist) = *ints;
      anim->will_fragment = 0;
      for (size i = 0; i < 24; i++) frame(anim);
    }

    anim->hist->items[anim->hist->len - 1] = *ints;

    frame(anim);

    if (((h) % 111) == 0) {
      new(arena, u8, 1);    // triggers relocation on next da_grow

      if (anim->will_fragment == 0) {
        anim->will_fragment = 1;
        for (size i = 0; i < 64; i++) {
          frame(anim);
        }
      }
      anim->will_fragment = 1;
    }
  }

  return 0;
}

/* Local Variables: */
/* compile-command: "cc ppm.c -o ppm -Wall -Wextra -ggdb3 -fsanitize=undefined -lm && ./ppm | mpv --no-correct-pts --fps=60 -" */
/* End: */
