/*
 * A linear allocator works well until we need resource cleanup (file handles, sockets, etc.)
 * C++ follows RAII paradigm and cleans up resources with a destructor when the resource gets out of scope.
 * So what if we implement a similar scope for the Arena?
 *
 * Idea taken from [[https://www.slideshare.net/DICEStudio/scope-stack-allocation][Scope Stack Allocator]].
 * Presentation from DICE Coder's Day (2010 November) by Andreas Fredriksson.
 */

// First lets implement a typical Arena

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef ptrdiff_t size;
typedef uint8_t u8;

#define alignof(s) (size)_Alignof(s)
#define sizeof(s)  (size)sizeof(s)
#define countof(s) (sizeof(s) / sizeof(*(s)))
#define assert(c)  while((!(c))) __builtin_unreachable()

#define new(a, t, n) (t *) arena_alloc(a, sizeof(t), alignof(t), (n))
typedef struct Arena {
  uint8_t *backing;
  size capacity;
  size at; // current allocator point
} Arena;

Arena *arena_bootstrap(u8 *backing, size capacity)
{
  Arena *a = (Arena *)backing;
  a->backing = backing;
  a->capacity = capacity;
  a->at = sizeof(Arena);
  return a;
}

__attribute__((malloc, alloc_size(2,4), alloc_align(3)))
u8 *arena_alloc(Arena *a, size objsize, size align, size count)
{
  size avail = a->capacity - a->at;
  size padding = -(uintptr_t)(a->backing + a->at) & (align - 1);
  size total   = padding + objsize * count;
  if (avail < total) {
    fprintf(stderr, "Out of Memory\n");
    exit(2);
  }

  u8 *p = (a->backing + a->at) + padding;
  a->at += total;

  memset(p, 0, objsize * count);

  return p;
}

void arena_rewind(Arena *a, size point)
{
  a->at = point;
}

// Next, we introduce the concept of a scope and keep track of a list of resources to cleanup.
// I typically implement a scope anyway to "free" temporary memory that I allocate in a scope.

typedef void (*Resource_Callback)(void*);

typedef struct Resource_Node Resource_Node;
struct Resource_Node {
  void *data;
  Resource_Callback cb;
  Resource_Node *next;
};

typedef struct Arena_Scope {
  Arena *arena;
  size marker;
  Resource_Node *destructors_list;
} Arena_Scope;

Arena_Scope arena_scope_begin(Arena *arena)
{
  Arena_Scope result = {0};
  result.arena = arena;
  result.marker = arena->at;
  return result;
}

void arena_scope_end(Arena_Scope scope)
{
  for (Resource_Node *r = scope.destructors_list; r; r = r->next) {
    r->cb(r->data);
  }
  arena_rewind(scope.arena, scope.marker);
}

void arena_scope_destructor(Arena_Scope *scope, void *data, Resource_Callback cb)
{
  Resource_Node *node = new(scope->arena, Resource_Node, 1);
  node->data = data;
  node->cb = cb;
  node->next = scope->destructors_list;
  scope->destructors_list = node;
}

// Acquire resources in a scope and observe the destructors being called.

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct MyFile {
  int fd;
  const char *filename;
} MyFile;

void close_file(void *data)
{
  MyFile *f = (MyFile *)data;
  printf("Closing file %s w/ fd %d\n", f->filename, f->fd);
  close(f->fd);
}

MyFile *open_file(Arena_Scope *s, const char *filename)
{
  int fd = open(filename, O_RDWR, 0755);
  if (fd < 0) {
    perror("open");
    exit(1);
  }

  MyFile *f = new(s->arena, MyFile, 1);
  f->filename = filename;
  f->fd = fd;
  arena_scope_destructor(s, f, close_file);

  printf("Opened file %s w/ fd %d\n", f->filename, f->fd);
  return f;
}

// Optional macro syntax sugar
#define MACRO_VAR(i) macro_i_##__LINE__
#define Scope(a, name) size MACRO_VAR(i) = 1; \
  for (Arena_Scope name = arena_scope_begin((a)); MACRO_VAR(i)--; arena_scope_end((name)))

int main(int argc, char **argv)
{
  size mem_size = 8 * 1024 * 1024;
  Arena *arena = arena_bootstrap(malloc(mem_size), mem_size);

  Scope(arena, outer_scope)
  {
    const char *files[] = {
      "./arena_resource_cleanup_ext.c",
      "./err.c",
      "./trie.c"
    };

    for (size i = 0; i < countof(files); i++) {
      MyFile *f = open_file(&outer_scope, files[i]);
    }

    Scope(arena, inner_scope)
    {
      for (size i = countof(files) - 1; i >= 0; i--) {
        MyFile *f = open_file(&inner_scope, files[i]);
      }
    }

    for (size i = 0; i < countof(files); i++) {
      MyFile *f = open_file(&outer_scope, files[i]);
    }
  }

  return 0;
}
