#if IN_SHELL /* $ bash generators.c
cc generators.c -o generators -fsanitize=undefined -g3 -Os -pedantic -Wall -Wextra -Wconversion -Wno-sign-conversion $@
exit # */
#endif

#define assert(c)        while (!(c)) __builtin_unreachable()
#define tassert(c)       while (!(c)) __builtin_trap()
#define breakpoint(c)    ((c) ? ({ asm volatile ("int3; nop"); }) : 0)
#define countof(a)       (Iz)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)     ((t *)arena_alloc(a, sizeof(t), _Alignof(t), (n)))
#define newbeg(a, t, n)  ((t *)arena_alloc_beg(a, sizeof(t), _Alignof(t), (n)))
#define s8(s)            (S8){(U8 *)s, countof(s)-1}
#define memcpy(d, s, n)  __builtin_memcpy(d, s, n)
#define memset(d, c, n)  __builtin_memset(d, c, n)

typedef unsigned char U8;
typedef signed long long I64;
typedef typeof((char *)0-(char *)0) Iz;
typedef typeof(sizeof(0))           Uz;

////////////////////////////////////////////////////////////////////////////////
//- Arena

typedef struct { U8 *beg;  U8  *end; } Arena;

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Iz objsize, Iz align, Iz count) {
  Iz padding = (Uz)a->end & (align - 1);
  tassert((count <= (a->end - a->beg - padding) / objsize) && "out of memory");
  Iz total = objsize * count;
  return memset(a->end -= total + padding, 0, total);
}

__attribute((malloc, alloc_size(4, 2), alloc_align(3)))
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

#define s8pri(s) (int)s.len, s.data

typedef struct { U8 *data; Iz len; } S8;

static S8 s8span(U8 *beg, U8 *end) { return (S8){beg, end - beg}; }

static S8 s8dup(Arena *a, S8 s) {
  return (S8) {
    memcpy((new(a, U8, s.len)), s.data, s.len * sizeof(U8)),
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

static S8 s8trimspace(S8 s) {
  for (Iz off = 0; off < s.len; off++) {
    _Bool is_ws = (s.data[off] == ' ' || ((unsigned)s.data[off] - '\t') < 5);
    if (!is_ws) { return (S8){s.data + off, s.len - off}; }
  }
  return s;
}

static _Bool s8match(S8 a, S8 b, Iz n) {
  if (a.len < n || b.len < n)  { return 0; }
  for (Iz i = 0; i < n; i++) {
    if (a.data[i] != b.data[i]) { return 0; }
  }
  return 1;
}

#define s8startswith(a, b) s8match((a), (b), (b).len)

static S8 s8tolower(S8 s) {
  for (Iz i = 0; i < s.len; i++) {
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
  Iz len = countof(digits) - i + negative;
  U8 *beg = newbeg(arena, U8, len);
  U8 *end = beg;
  if (negative) { *end++ = '-'; }
  do { *end++ = digits[i++]; } while (i < countof(digits));
  return (S8){beg, len};
}

////////////////////////////////////////////////////////////////////////////////
//- Error side channel

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

static ErrList *errors_make(Arena *arena, Iz nbyte) {
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

#define for_errors(varname)                                                    \
  for (Iz _defer_i_ = 1; _defer_i_; _defer_i_--, errors_clear_())            \
    for (Err *varname = errors->first; varname && (errors->max_severity > 0);  \
         varname = varname->next)

static Err *emit_err(int severity, S8 message) {
  assert(errors && errors->arena.beg);
  if ((errors->arena.end - errors->arena.beg) <
      ((Iz)sizeof(Err) + message.len + (1 << 8))) {
    errors_clear_(); // REVIEW: force flush errors to stderr?
    emit_err(3, s8("Exceeded error memory limit. Previous errors omitted."));
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

#define emit_errno(arena, ...)                                                 \
  do {                                                                         \
    S8 msg = {0};                                                              \
    msg = s8concat(arena, msg, __VA_ARGS__, s8(": "),                          \
                   s8cstr(&scratch, strerror(errno)));                         \
    emit_err(3, msg);                                                          \
  } while (0);


////////////////////////////////////////////////////////////////////////////////
//- File I/O

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
    Iz len = 0;
    {
      Arena scratch = *arena;
      char *f = s8z(&scratch, file);
      if ((fd = open(f, O_RDONLY)) < 0) {
        emit_errno(&scratch, s8("open '"), file, s8("'"));
        return r;
      }
      if ((len = lseek(fd, 0, SEEK_END)) < 0) {
        emit_errno(&scratch, s8("lseek '"), file, s8("'"));
        goto defer;
      }
      if (lseek(fd, 0, SEEK_SET) < 0) {
        emit_errno(&scratch, s8("lseek '"), file, s8("'"));
        goto defer;
      }
    }

    U8 *beg = new(arena, U8, len + 1); // note: "+ 1" for '\0'
    U8 *end = beg;
    {
      Arena scratch = *arena;
      Iz m = len;
      while (m > 0) {
        Iz nbyte = 0;
        do {
          nbyte = read(fd, end, m);
        } while (nbyte < 0 && errno == EINTR);
        if (nbyte < 0) {
          emit_errno(&scratch, s8("read '"), file, s8("'"));
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

static void write_file(Arena scratch, S8 file, S8 file_content) {
  if (file.data && file_content.len) {
    int fd = open(s8z(&scratch, file), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
      emit_errno(&scratch, s8("open '"), file, s8("'"));
      return;
    }

    while (file_content.len > 0) {
      Iz m = file_content.len;
      Iz nbyte = 0;
      do {
        nbyte = write(fd, file_content.data, m);
      } while (nbyte < 0 && errno == EINTR);
      if (nbyte < 0) {
        emit_errno(&scratch, s8("write '"), file, s8("'"));
        goto defer;
      }
      file_content.data += nbyte;
      file_content.len -= nbyte;
    }

  defer:
    close(fd);
  }
}

////////////////////////////////////////////////////////////////////////////////
//- Generator File Chunks

typedef struct {
  U8 *beg, *end; // region safe to change
  S8 command;
  S8 generated;
} Chunk;

typedef struct {
  Chunk *items;
  Iz capacity;
  Iz count;
} Chunks;

//{generate dynamic_array Chunks Chunk
static void chunks_grow(Arena *a, Chunks *slice) {
  slice->capacity += !slice->capacity;
  Chunk *data = new(a, Chunk, slice->capacity * 2);
  if (slice->count)
    memcpy(data, slice->items, sizeof(Chunk) * slice->count);
  slice->items = data;
  slice->capacity *= 2;
}

static Chunk *chunks_push(Arena *a, Chunks *slice) {
  if (slice->count >= slice->capacity)
    chunks_grow(a, slice);
  return slice->items + slice->count++;
}
//}

////////////////////////////////////////////////////////////////////////////////
//- Program

static S8 maybe_generate_dynamic_array(Arena *arena, S8 cmd) {
  if (s8startswith(cmd, s8("dynamic_array"))) {
    S8pair tokens = s8cut(cmd, ' ');
    S8 type         = (tokens = s8cut(tokens.tail, ' ')).head;
    S8 item_type    = (tokens = s8cut(tokens.tail, ' ')).head;
    S8 items_member = (tokens = s8cut(tokens.tail, ' ')).head;
    S8 cap_member   = (tokens = s8cut(tokens.tail, ' ')).head;
    S8 count_member = (tokens = s8cut(tokens.tail, ' ')).head;

    if (type.len == 0) {
      emit_err(3, s8("Expected container type as 1st argument"));
      return (S8){0};
    }
    if (item_type.len == 0) {
      emit_err(3, s8("Expected item-type as 2nd argument"));
      return (S8){0};
    }

    if (items_member.len == 0) items_member = s8("items");
    if (  cap_member.len == 0)   cap_member = s8("capacity");
    if (count_member.len == 0) count_member = s8("count");

    S8 type_lower = s8tolower(s8dup(arena, type));

    S8 buf = {0};
    {
      // clang-format off
      buf =
        s8concat(arena, buf,
                 s8("static void "), type_lower, s8("_grow"),
                 s8("(Arena *a, "), type, s8(" *slice"), s8(") {\n"),
                 s8("  slice->"), cap_member, s8(" += !slice->"), cap_member, s8(";\n"),
                 s8("  "), item_type, s8(" *data = new(a, "), item_type, s8(", slice->"), cap_member, s8(" * 2);\n"),
                 s8("  if (slice->"), count_member, s8(")\n"),
                 s8("    memcpy(data, slice->"), items_member, s8(", sizeof("), item_type, s8(") * slice->"), count_member, s8(");\n"),
                 s8("  slice->"), items_member, s8(" = data;\n"),
                 s8("  slice->"), cap_member, s8(" *= 2;\n"),
                 s8("}\n"));
      // clang-format on
    }
    buf = s8concat(arena, buf, s8("\n"));
    {
      // clang-format off
      buf =
        s8concat(arena, buf,
                 s8("static "), item_type, s8(" *"), type_lower, s8("_push"),
                 s8("(Arena *a, "), type, s8(" *slice"), s8(") {\n"),
                 s8("  if (slice->"), count_member, s8(" >= slice->"), cap_member, s8(")\n"),
                 s8("    "), type_lower, s8("_grow(a, slice);\n"),
                 s8("  return slice->"), items_member, s8(" + slice->"), count_member, s8("++;\n"),
                 s8("}\n"));
      // clang-format on
    }
    return buf;
  }
  return (S8){0};
}

static S8 generate(Arena *arena, S8 file_content)
{
  //- Extract generator chunks from source
  Chunks chunks = {0};
  {
    Iz curidx = -1;
    S8pair line = (S8pair){{0}, file_content};
    while (line.tail.data) {

      line = s8cut(line.tail, '\n');
      S8 cmd = s8trimspace(line.head);

      if (curidx < 0) {
        S8 match = s8("//{generate ");
        if (s8startswith(cmd, match)) {
          Chunk *chunk = chunks_push(arena, &chunks);
          chunk->command = s8span(cmd.data + match.len, cmd.data + cmd.len);
          curidx = chunks.count - 1;
        }
      } else {
        Chunk *chunk = &chunks.items[curidx];
        if (s8startswith(cmd, s8("//}"))) {
          if (chunk->beg == 0) chunk->beg = line.head.data;
          chunk->end = line.head.data;
          curidx = -1;
        }
        else if (chunk->beg == 0 && !s8startswith(cmd, s8("//"))) {
          chunk->beg = line.head.data;
        }
      }
    }
    if (curidx >= 0) {
      emit_err(3, s8("Generator missing ending delimiter"));
      return (S8){0};
    }
  }

  //- Generate content for chunks
  for (Iz i = 0; i < chunks.count; i++) {
    Chunk *chunk = &chunks.items[i];
    chunk->generated = maybe_generate_dynamic_array(arena, chunk->command);

    if (chunk->generated.len == 0) {
      Iz file_offset = (chunk->beg - file_content.data);
      emit_err(1, s8concat(arena, s8("Unknown generator at file offset "), s8i64(arena, file_offset)));
    }
  }

  //- Build content with evaluated generators
  S8 output = {0};
  {
    U8 *beg = file_content.data;
    U8 *end = beg;
    for (Iz i = 0; i < chunks.count; i++) {
      Chunk *chunk = &chunks.items[i];
      end = chunk->beg;
      S8 above = s8span(beg, end);
      output = s8concat(arena, output, above);
      if (chunk->generated.len) {
        output = s8concat(arena, output, chunk->generated);
      } else {
        S8 leave_unmodified = s8span(chunk->beg, chunk->end);
        output = s8concat(arena, output, leave_unmodified);
      }
      beg = chunk->end;
    }
    end = file_content.data + file_content.len;
    output = s8concat(arena, output, s8span(beg, end));
  }

  return output;
}

#define HEAP_CAP (1u << 28)

__thread struct ErrList *errors;

#if !__AFL_COMPILER
int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr, "ERROR: no input file.\n");
    return 1;
  }

  U8 *heap = malloc(HEAP_CAP);
  Arena arena[1] = { (Arena){heap, heap + HEAP_CAP}, };

  errors = errors_make(arena, 1 << 12);

  S8 file   = s8cstr(arena, argv[1]);
  S8 backup = s8concat(arena, file, s8(".backup"));
  S8 file_content = read_entire_file(arena, file);
  S8 new_content = generate(arena, file_content);
  write_file(*arena, backup, file_content);
  write_file(*arena, file, new_content);

  int status_code = !!errors->max_severity;
  for_errors(err) {
    fprintf(stderr, "[ERROR]: %.*s\n", s8pri(err->message));
  }
  return status_code;
}
#endif


#ifdef __AFL_COMPILER

#include <sys/mman.h>
#include <unistd.h>

__AFL_FUZZ_INIT();

int main() {
  __AFL_INIT();
  U8 *heap = malloc(HEAP_CAP);
  assert(heap);
  Arena arena[1] = { (Arena){heap, heap + HEAP_CAP}, };

  errors = make_errors(arena, 1 << 12);

  int input  = memfd_create("fuzz_in",  0);
  int output = memfd_create("fuzz_out", 0);
  assert(input  == 3 && "We assume input file gets located at /proc/self/fd/3");
  assert(output == 4 && "We assume input file gets located at /proc/self/fd/4");

  unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
  while (__AFL_LOOP(10000)) {
    int len = __AFL_FUZZ_TESTCASE_LEN;
    ftruncate(input, 0);
    pwrite(input, buf, len, 0);

    S8 file_content = read_entire_file(arena, s8("/proc/self/fd/3"));
    S8 new_content = generate(arena, file_content);
    write_file(*arena, s8("/proc/self/fd/4"), new_content);
    for_errors(err) {
      fprintf(stderr, "[ERROR]: %.*s\n", s8pri(err->message));
    }
  }
}

#endif
