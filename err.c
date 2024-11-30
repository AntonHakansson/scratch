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

#define I64_MIN		(-__INT64_C(9223372036854775807)-1)
#define I64_MAX		( __INT64_C(9223372036854775807))

////////////////////////////////////////////////////////////////////////////////
//- Platform Agnostic Layer
// REVIEW: We should start allowing str slices, and other common program abstractions, in the platform API.
//           - Str, Arena, Write_Buffer, etc.

#define HEAP_CAPACITY (1<<28)
b32  os_write(i32 fd, u8 *buf, size len);
void os_exit (i32 status);

////////////////////////////////////////////////////////////////////////////////
//- Arena Allocator

#define new(...) new_(__VA_ARGS__, new3, new2)(__VA_ARGS__)
#define new_(a,b,c,d,...) d
#define new2(a, t)    (t *) arena_alloc(a, sizeof(t), alignof(t), 1)
#define new3(a, t, n) (t *) arena_alloc(a, sizeof(t), alignof(t), (n))

typedef struct {
  // REVIEW: We only need an *at and *end pointer
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
    os_exit(1);
  }

  u8 *p = a->at + padding;
  a->at += total;

  for (size i = 0; i < objsize * count; i++) {
    p[i] = 0;
  }

  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- String slice

#define S(s)        (Str){ .buf = (u8 *)(s), .len = countof((s)) - 1, }

typedef struct {
  u8 *buf;
  size len;
} Str;

int os_write_str(int fd, Str s)
{
  return os_write(fd, s.buf, s.len);
}

////////////////////////////////////////////////////////////////////////////////
//- Error free API
//
// Arena friendly error API that supports dynamic error messages and semantics similar to
// try/catch but without the control flow headache.
// The API allows users to start a new scope to accumulate errors that gets consolidated
// later. This can simplify and collapse almost all error checking code paths. However,
// the called procedure needs to accommodate this design choice by first handling invalid
// input gracefully, and second, emit errors through the API.
//
// Compare (traditional)
//
// Window  *window = open_window(...);
// if (!window) { ERROR }
// Open_GL *gl = init_opengl(window, ...);
// if (!gl) { ERROR }
//
// with (Error-free API)
//
// Error_Scope err_scope = err_start("FATAL");
// {
//   Window  *window = open_window(..., &err_scope);
//   Open_GL *gl     = init_opengl(window, ..., &err_scope);
// }
// Error_Node *e = err_end(err_scope);
// for (; e; e = e->next) { ERROR e->text }
//

// For special errors that include information that the user might want to handle
// separately.  Line locations for parsing functions etc.
typedef enum Error_Type {
  Error_Type_XXX, // ...
  Error_Type_Parser
} Error_Type;

typedef struct Error_Node {
  struct Error_Node *next;
  Str text;

  Error_Type type;
  union {
    struct { int x; } XXX;
    struct { Str file_name; int col, row; } parser;
  };
} Error_Node;

typedef struct Error_Scope {
  // Previous node before opening the scope. If null then we are the root scope.
  Error_Node *prev;
  Str  name;
} Error_Scope;

typedef struct Error_Vars {
  Error_Node *first;
  Error_Node *last;
} Error_Vars;

__thread Error_Vars global_err_vars = {0};

Error_Scope err_start(Str scope_name)
{
  Error_Vars *err = &global_err_vars;

  Error_Scope r = {0};
  r.name  = scope_name;
  r.prev  = err->last;
  return r;
}

Error_Node * err_end(Error_Scope err_scope)
{
  Error_Vars *err = &global_err_vars;
  Error_Node *result = err_scope.prev ? err_scope.prev->next : err->first;

  err->last = err_scope.prev;
  if (err->last) { err->last->next = 0; }

  return result;
}

Error_Node * err_emit(Arena *arena, Str error_text)
{
  Error_Vars *err = &global_err_vars;

  Error_Node *node = new(arena, Error_Node);
  node->text = error_text;

  if (err->first == 0) {
    err->first = node;
    err->last = node;
  }
  else {
    err->last->next = node;
    err->last = node;
  }

  return node;
}

// Write_Buffer *write_buffer = err_buff(arena, 1024);
// append_str(write_buffer, S("[ERROR]: "))
// append_str(write_buffer, file_path)

////////////////////////////////////////////////////////////////////////////////
//- Program

i32 run(Arena *arena, i32 argc, char *argv[])
{
  /* size stdout_capacity = 8 * 1024; */
  /* Write_Buffer stdout[1] = { fd_buffer(1, new(perm, u8, stdout_capacity), stdout_capacity) }; */

  Error_Scope err_scope = err_start(S("str_operations"));
  err_emit(arena, S("Can't find file"));

  {
    Error_Scope err_scope = err_start(S("Some other scope for accumulating errors"));
    err_emit(arena, S("Error that will get ignored by the developer for some reason"));
    err_end(err_scope);
  }

  err_emit(arena, S("Some other error"));

  Error_Node *e = err_end(err_scope);
  for (; e; e = e->next) {
    os_write_str(2, S("[Error]: "));
    os_write_str(2, e->text);
    os_write_str(2, S("\n"));
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//- libc platform

#include <stdlib.h>
#include <unistd.h>

b32 os_write(i32 fd, u8 *buf, size len)
{
  for (size off = 0; off < len;) {
    size written = write(fd, buf + off, len - off);
    if (written < 1) { return 0; }
    off += written;
  }
  return 1;
}

void os_exit(int status)
{
  exit(status);
}

int main(int argc, char **argv)
{
  Arena heap[1] = { arena_init((u8 *)malloc(HEAP_CAPACITY), HEAP_CAPACITY) };
  return run(heap, argc, argv);
}

/* Local Variables: */
/* outline-regexp: " *\/\/\\(-+\\)" */
/* End: */
