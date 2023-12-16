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
#define alignof(s) (size)_Alignof(s)
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
// the old array.  The old array is preserved on the arena.  Be catious about holding
// pointers as they refer to old data after relocation.
//
// The design is composable such that it works with any struct that has a items, capacity,
// and len member.
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

// Assumes items fit in capacity
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
//- Program

typedef struct i32s {
  i32 *items;
  size capacity;
  size len;
} i32s;

#ifdef TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  size heap_size = (1 << 18);
  u8  *heap      = malloc(heap_size);

  Arena arena[1] = { arena_init(heap, heap_size) };

  i32s *ints = da_init(arena, i32s, 2);
  *da_push_unsafe(ints) = 6969;
  *da_push_unsafe(ints) = 420;

  size total_fragmented_capacity = ints->capacity;
  size insertions = 300;
  for (size i = 0, h = 0x100; i < insertions; i += 1, h *= 1111111111111111111u) {
    i32s prev_ints = *ints;
    *da_push(arena, ints) = abs((int)h) % 1000;

    b32 did_grow = (ints->items != prev_ints.items);
    if (did_grow) {
      total_fragmented_capacity += ints->capacity;
      printf("%ld: relocated %lu -> %lu\n", i + 1, prev_ints.capacity, ints->capacity);
    }

    if (((i + 1) % 32) == 0) {
      printf("%ld: fragment %ld\n", i + 1, ints->capacity);
      new(arena, u8, 1);    // triggers relocation on next da_grow
    }
  }
  putc('\n', stdout);

  printf("Insertions %lu\n", insertions);
  printf("Final Capacity: %lu B\n", (ints->capacity * (sizeof(*ints->items))));
  printf("Total Fragmented Capacity: %lu B\n", total_fragmented_capacity * (sizeof(*ints->items)));
  printf("Arena bytes committed: %lu\n", (arena->at - arena->backing));

  return 0;
}


#elif defined(FUZZ)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
  size heap_size = (1 << 18);
  u8 *heap = malloc(heap_size);

  Arena arena[1] = { arena_init(heap, heap_size) };

  i32s *ints = da_init(arena, i32s, 2);
  *da_push_unsafe(ints) = 6969;
  *da_push_unsafe(ints) = 420;

  #define buf_len 256
  int buf[buf_len];
  fread(buf, 1, sizeof(buf)-1, stdin);

  u32 insertions = buf[0];
  insertions = insertions & ((1<<10)-1);

  for (u32 i = 0; i < insertions; i++) {
    *da_push(arena, ints) = buf[i % buf_len];
    if ((ints->items[i] % 20) == 0) {
      new(arena, u8, 1);
    }
  }

  assert(ints->len == (insertions + 2));

  return 0;
}

#endif


/* Local Variables: */
/* compile-command: "cc dynarr.c -o dynarr -Wall -Wextra -ggdb -fsanitize=undefined -DTEST && ./dynarr" */
/* End: */
