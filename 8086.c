#if IN_SHELL /* $ bash 8086.c
cc 8086.c -o 8086 -fsanitize=undefined -g3 -O0 -std=c2x -Wall -Wextra -Wconversion -Wno-sign-conversion $@
exit # */
#endif

#define assert(c)        while (!(c)) __builtin_unreachable()
#define tassert(c)       while (!(c)) __builtin_trap()
#define breakpoint(c)    ((c) ? ({ __asm__ volatile ("int3; nop"); }) : 0)
#define countof(a)       (Size)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)     ((t *)arena_alloc(a, sizeof(t), _Alignof(t), (n)))
#define newbeg(a, t, n)  ((t *)arena_alloc_beg(a, sizeof(t), _Alignof(t), (n)))

typedef unsigned char U8;
typedef unsigned short U16;
typedef signed long long I64;
typedef typeof((char *)0-(char *)0) Size;
typedef typeof(sizeof(0))           USize;

////////////////////////////////////////////////////////////////////////////////
//- Arena

typedef struct { U8 *beg;  U8  *end; } Arena;

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static U8 *
arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = (USize)a->end & (align - 1);
  tassert((count <= (a->end - a->beg - padding) / objsize) && "out of memory");
  Size total = objsize * count;
  return __builtin_memset(a->end -= total + padding, 0, total);
}

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static U8 *
arena_alloc_beg(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
  tassert(total < (a->end - a->beg) && "out of memory");
  U8 *p = a->beg + padding;
  __builtin_memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- String

#define s8(s)            (S8){(U8 *)s, countof(s)-1}

typedef struct { U8 *data; Size len; } S8;

static S8 s8span(U8 *beg, U8 *end) { return (S8){beg, end - beg}; }

static S8 s8dup(Arena *a, S8 s) {
  return (S8) {
    __builtin_memcpy((new(a, U8, s.len)), s.data, s.len * sizeof(U8)),
    s.len,
  };
}

static char *s8z(Arena *a, S8 s) {
  return __builtin_memcpy(new(a, char, s.len + 1), s.data, s.len);
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
    if (head.len) __builtin_memcpy(copy.data, head.data, head.len);
    head = copy;
  }
  for (Size i = 0; i < count; i++) {
    S8 tail = ss[i];
    U8 *data = newbeg(a, U8, tail.len);
    if (tail.len) __builtin_memcpy(data, tail.data, tail.len);
    head.len += tail.len;
  }
  r = head;
  return r;
}

static S8 s8fmt(Arena *arena, const char *fmt, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, fmt);
  char *beg = (char *)arena->beg;
  arena->beg += __builtin_vsnprintf(beg, arena->end - arena->beg, fmt, args);
  __builtin_va_end(args);
  return s8span((U8 *)beg, arena->beg);
}

////////////////////////////////////////////////////////////////////////////////
//- Errors side channel

extern __thread struct ErrList *errors;

typedef struct Err Err;
struct Err {
  Err *next;
  int severity;
  S8 message;
};

typedef struct ErrList {
  Arena rewind_arena; // original [beg, end) range for rewinding
  Arena arena;
  Err *first;
  int max_severity;
} ErrList;

static ErrList *errors_make(Arena *arena, Size nbyte) {
  assert(errors == 0);
  ErrList *r = new(arena, ErrList, 1);
  U8 *beg = new(arena, U8, nbyte);
  r->arena = r->rewind_arena = (Arena){beg, beg + nbyte};
  return r;
}

static void errors_clear_() {
  errors->first = 0;
  errors->max_severity = 0;
  errors->arena = errors->rewind_arena;
}

static S8 err_s8severity(Err *e) {
  S8 r = {0};
  switch(e->severity) {
  default: { assert(0 && "unreachable"); }
  case 1: { r = s8("DEBUG"); } break;
  case 2: { r = s8("WARNING"); } break;
  case 3: { r = s8("ERROR"); } break;
  }
  return r;
}

static Err *err_emit(int severity, S8 message) {
  assert(errors && errors->arena.beg);
  if ((errors->arena.end - errors->arena.beg) <
      ((Size)sizeof(Err) + message.len + (1 << 8))) {
    errors_clear_(); // REVIEW: force flush errors to stderr?
    err_emit(3, s8("Exceeded error memory limit. Previous errors omitted."));
  }

  Err *err = new(&errors->arena, Err, 1);
  err->severity = severity;
  err->message = s8dup(&errors->arena, message);
  err->next = errors->first;
  errors->first = err;
  if (severity > errors->max_severity) {
    errors->max_severity = severity;
  }
  return err;
}

#define for_errors(varname)                                                    \
  for (Size _defer_i_ = 1; _defer_i_; _defer_i_--, errors_clear_())            \
    for (Err *varname = errors->first; varname && (errors->max_severity > 0);  \
         varname = varname->next)

#define errno_emit(arena, ...)                                                 \
  do {                                                                         \
    S8 msg = {0};                                                              \
    msg = s8concat(arena, msg, __VA_ARGS__, s8(": "),                          \
                   s8cstr(&scratch, strerror(errno)));                         \
    err_emit(3, msg);                                                          \
  } while (0);


////////////////////////////////////////////////////////////////////////////////
//- Program
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static S8 read_entire_file(Arena *arena, S8 file) {
  S8 r = {0};
  if (file.data) {
    int fd = 0;
    Size len = 0;
    {
      Arena scratch = *arena;
      char *f = s8z(&scratch, file);
      if ((fd = open(f, O_RDONLY)) < 0) {
        errno_emit(&scratch, s8("open '"), file, s8("'"));
        return r;
      }
      if ((len = lseek(fd, 0, SEEK_END)) < 0) {
        errno_emit(&scratch, s8("lseek '"), file, s8("'"));
        goto defer;
      }
      if (lseek(fd, 0, SEEK_SET) < 0) {
        errno_emit(&scratch, s8("lseek '"), file, s8("'"));
        goto defer;
      }
    }

    U8 *beg = new(arena, U8, len + 1); // note: extra byte is '\0'
    U8 *end = beg;
    {
      Arena scratch = *arena;
      Size m = len;
      while (m > 0) {
        Size nbyte = 0;
        do {
          nbyte = read(fd, end, m);
        } while (nbyte < 0 && errno == EINTR);
        if (nbyte < 0) {
          errno_emit(&scratch, s8("write '"), file, s8("'"));
          goto defer;
        }
        end += nbyte;
        m   -= nbyte;
      }
    }
    r = s8span(beg, end);

  defer:
    close(fd);
  }
  return r;
}

__thread struct ErrList *errors;

int main(int argc, char **argv) {
  tassert(argc > 1);

  Size heap_capacity = 1u << 28;
  U8 *heap = malloc(heap_capacity);
  Arena arena[1] = {(Arena){heap, heap + heap_capacity}};

  errors = errors_make(arena, 1u << 12);

  S8 infile = read_entire_file(arena, s8cstr(arena, argv[1]));

  U8 *instr = infile.data;
  U8 *mem = new(arena, U8, 1<<10);
  U16 *registers = new(arena, U16, 16);

  //- Instruction Decode
  if (errors->max_severity == 0) {

    U8 *at = instr;
    while (at < (infile.data + infile.len)) {

      breakpoint(1);
      U8 opcode = *at++;
      if (0) {
      } else if ((opcode >> 4) == 0b1011) {
        _Bool wide = opcode & 0b1000;
        U8 reg = opcode & 0b111;

        U8 low = *at++;
        if (wide) {
          U8 high = *at++;
          registers[reg] = (high << 8) | low;
        }
        else {
          registers[reg] = registers[reg] & 0xF0 | low;
        }
      }
      else if ((opcode >> 2) == 0b100010) {
        _Bool dst = opcode & 0b01;
        _Bool wide      = opcode & 0b10;

        U8 operands = *at++;

        U8 mod = (operands >> 6) & 0b011;
        U8 reg = (operands >> 2) & 0b111;
        U8 rm  = (operands >> 0) & 0b111;
        tassert(mod == 0b11); // register to register

        if (mod == 0b11) {
          if (dst) {/* reg <- rm */}
          else {
            // rm <- reg
            U8 tmp = reg;
            reg = rm;
            rm = tmp;
          }

          if (wide) {
            registers[reg] = registers[rm];
          }
          else {
            U8 *dst = (U8 *)&registers[reg];
            dst += !!(reg & 0b100);

            U8 *src = (U8 *)&registers[rm];
            src += !!(rm & 0b100);

            *dst = *src;
          }
        }
      } else {
        S8 m = s8fmt(arena, "Unknown instruction %X\n", opcode);
        err_emit(2, m);
      }
    }
  }

  for (Size i = 0; i < 16; i++) {
    printf("%X\n", registers[i]);
  }
  

  for_errors(err) {
    S8 severity = err_s8severity(err);
    fprintf(stderr, "[%.*s]: %.*s\n", (int)severity.len, severity.data,
            (int)err->message.len, err->message.data);
  }

  free(heap);
  return 0;
}
