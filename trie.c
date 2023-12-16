// Platform: POSIX libc
//
// $ cc trie.c -o trie
// $ ./trie   # requires dot and ffmpeg in path
//

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

#define HEAP_CAPACITY (1<<28)

i32  os_open (char *file_path);
i32  os_close(i32 fd);
b32  os_write(i32 fd, u8 *buf, size len);
b32  os_read (i32 fd, u8 *buf, size len);
void os_exit (i32 status);
i32  os_mkdir(char *file_path);

typedef struct Pipe {
  i32 write_fd;
  i32 pid;
} Pipe;

Pipe os_start_graphviz(char *out_file);
b32  os_stop_graphviz(Pipe pipe);
b32  os_ffmpeg(char *out_file);


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
#define S_PRINT(s)  (i32)s.len, s.buf
#define S_FMT       "%.*s"

typedef struct {
  u8 *buf;
  size len;
} Str;

b32 str_equals(Str a, Str b)
{
  if (a.len != b.len) { return 0; }
  for (size i = 0; i < a.len; i++) {
    if (a.buf[i] != b.buf[i]) { return 0; }
  }
  return 1;
}

////////////////////////////////////////////////////////////////////////////////
//- Parsing

typedef enum Parse_Status {
  Parse_Status_Ok,
  Parse_Status_Invalid,
  Parse_Status_Too_Large,
  Parse_Status_Too_Small,
} Parse_Status;

typedef struct Parse_Result {
  Parse_Status status;
  i64 val;
} Parse_Result;

#define parse_i64(s) parse_i64_ex((s), I64_MIN, I64_MAX)

// Base 10, skips leading whitespace, handles +/- sign
// TODO: respect slice len because the string might not have a null terminator - otherwise "1231\0x12\0x11" would get parsed.
Parse_Result parse_i64_ex(Str s, i64 min, i64 max)
{
  assert(s.buf);
  assert(min < max);

  Parse_Result result = {0};
  u8 *buf = s.buf;

  // Skip any leading whitespace
  for (; (*buf >= 0x09 && *buf <= 0x0d) || (*buf == 0x20); buf++);

  i64 mmax, mmin, n = 0;
  switch (*buf) {
  case 0x2b: // +
    buf++; // fallthrough
  case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
  case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
    // Accumulate in positive direction, watching for max bound
    mmax = max / 10;
    do {
      int v = *buf++ - 0x30;

      if (v < 0 || v > 9) {
        goto invalid;
      }

      if (n > mmax) {
        goto toolarge;
      }
      n *= 10;

      if (max - v < n) {
        goto toolarge;
      }
      n += v;
    } while (*buf);

    // Still need to check min bound
    if (n < min) {
      goto toosmall;
    }

    result.val = n;
    return result;

  case 0x2d: // -
    buf++;
    // Accumulate in negative direction, watching for min bound
    mmin = min / 10;
    do {
      int v = *buf++ - 0x30;

      if (v < 0 || v > 9) {
        goto invalid;
      }

      if (n < mmin) {
        goto toosmall;
      }
      n *= 10;

      if (min + v > n) {
        goto toosmall;
      }
      n -= v;
    } while (*buf);

    // Still need to check max bound
    if (n > max) {
      goto toolarge;
    }

    result.val = n;
    return result;
  }

 invalid:
  result.status = Parse_Status_Invalid;
  return result;

 toolarge:
  // Skip remaining digits
  for (; *buf >= 0x30 && *buf <= 0x39; buf++);
  if (*buf) goto invalid;
  result.status = Parse_Status_Too_Large;
  return result;

 toosmall:
  // Skip remaining digits
  for (; *buf >= 0x30 && *buf <= 0x39; buf++);
  if (*buf) goto invalid;
  result.status = Parse_Status_Too_Small;
  return result;
}

////////////////////////////////////////////////////////////////////////////////
//- Buffered IO

typedef struct  {
  u8 *buf;
  i32 capacity;
  i32 len;
  i32 fd;
  _Bool error;
} Write_Buffer;

Write_Buffer write_buffer(u8 *buf, i32 capacity);
Write_Buffer fd_buffer(i32 fd, u8 *buf, i32 capacity);
void append(Write_Buffer *b, unsigned char *src, i32 len);
void flush();

Write_Buffer write_buffer(u8 *buf, i32 capacity)
{
  Write_Buffer result = {0};
  result.buf = buf;
  result.capacity = capacity;
  result.fd = -1;
  return result;
}

Write_Buffer fd_buffer(i32 fd, u8 *buf, i32 capacity)
{
  Write_Buffer result = {0};
  result.buf = buf;
  result.capacity = capacity;
  result.fd = fd;
  return result;
}

void append(Write_Buffer *b, unsigned char *src, i32 len)
{
  unsigned char *end = src + len;
  while (!b->error && src<end) {
    i32 left = end - src;
    i32 avail = b->capacity - b->len;
    i32 amount = avail<left ? avail : left;

    for (i32 i = 0; i < amount; i++) {
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

void append_str(Write_Buffer *b, Str s)
{
  append(b, s.buf, s.len);
}

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

void flush(Write_Buffer *b)
{
  b->error |= b->fd < 0;
  if (!b->error && b->len) {
    b->error |= !os_write(b->fd, b->buf, b->len);
    b->len = 0;
  }
}


////////////////////////////////////////////////////////////////////////////////
//- Hash Trie

typedef struct Hash_Map {
  struct Hash_Map *child[4];
  Str key;
  Str value;
} Hash_Map;

u64 hash_str(Str s)
{
  u64 h = 0x100;
  for (size i = 0; i < s.len; i++) {
    h ^= s.buf[i];
    h *= 1111111111111111111u;
  }
  return h;
}

Str *upsert(Arena *perm, Hash_Map **m, Str key)
{
  for (u64 h = hash_str(key); *m; h <<= 2) {
    if (str_equals(key, (*m)->key)) {
      return &(*m)->value;
    }
    m = &(*m)->child[h >> 62];
  }

  if (!perm) {
    return 0;
  }

  *m = new(perm, Hash_Map);
  (*m)->key = key;
  return &(*m)->value;
}


////////////////////////////////////////////////////////////////////////////////
//- Program

#define EXPORT_EMPTY_SLOTS 0

__attribute__((noreturn))
void fatal(Str err_msg) {
  os_write(2, err_msg.buf, err_msg.len);
  os_exit(1);
  assert(0 && "unreachable");
}

Str str_from_int(Arena *perm, i32 x)
{
  assert(x >= 0);

  Str result = {0};
  result.len = 1 + (x>9) + (x>99) + (x>999) + (x>9999) + (x>99999) + (x>999999) + (x>9999999);
  result.buf = new(perm, u8, result.len);
  u8 *s = result.buf;
  switch (result.len) {
  case 8: s[7] = '0' + x%10; x /= 10;  // fallthrough
  case 7: s[6] = '0' + x%10; x /= 10;  // fallthrough
  case 6: s[5] = '0' + x%10; x /= 10;  // fallthrough
  case 5: s[4] = '0' + x%10; x /= 10;  // fallthrough
  case 4: s[3] = '0' + x%10; x /= 10;  // fallthrough
  case 3: s[2] = '0' + x%10; x /= 10;  // fallthrough
  case 2: s[1] = '0' + x%10; x /= 10;  // fallthrough
  case 1: s[0] = '0' + x%10;
  }
  return result;
}

void append_hash_tree(Write_Buffer *b, Hash_Map *h)
{
  // Give each child edge a different color
  Str col_lut[4] = {
    S("red"), S("blue"), S("green"), S("magenta"),
  };

  append_str(b, h->key);
  append_lit(b, ";\n");

  for (size i = 0; i < 4; i++) {
    if (h->child[i]) {
      append_str(b, h->key);
      append_lit(b, " -> ");
      append_str(b, h->child[i]->key);
      append_lit(b, " [ color=");
      append_str(b, col_lut[i]);
      append_lit(b, " ]");
      append_lit(b, ";\n");
    }
    else {
      #if EXPORT_EMPTY_SLOTS
      u8 buf[8];
      Arena temp = { .backing =  buf, .capacity = countof(buf), .at = buf};
      static i32 rand_label = 0;
      Str empty = str_from_int(&temp, rand_label++);

      append_lit(b, "e");
      append_str(b, empty);
      append_lit(b, " [ label=\"\" style=\"dashed\" ]");
      append_lit(b, ";\n");

      append_str(b, h->key);
      append_lit(b, " -> e");
      append_str(b, empty);
      append_lit(b, " [ style=\"dashed\" ]");
      append_lit(b, ";\n");
      #endif
    }
  }

  for (size i = 0; i < 4; i++) {
    if (h->child[i]) {
      append_hash_tree(b, h->child[i]);
    }
  }
}

i32 run(Arena *perm, i32 argc, char *argv[])
{
  size stdout_capacity = 8 * 1024;
  Write_Buffer stdout[1] = { fd_buffer(1, new(perm, u8, stdout_capacity), stdout_capacity) };

  // Parse cli arguments
  b32 create_anim = (argc > 1);
  size iterations = 32;
  if (create_anim) {
    Str arg = (Str){.buf = (u8 *)argv[1], .len = -1, };
    Parse_Result r = parse_i64_ex(arg, 1, 999);
    if (r.status != 0) {
      // TODO: Improve error message
      append_lit(stdout, "Failed to parse number: ");
      append_long(stdout, r.status);
      append_byte(stdout, '\n');
      flush(stdout);
      os_exit(1);
    }
    iterations = r.val;
  }

  Str dir_path = S("./out/");
  if (create_anim) {
    os_mkdir((char *)dir_path.buf);
  }

  {
    Hash_Map *h = 0;
    for (size i = 0; i < iterations ; i++) {
      Str key = str_from_int(perm, i);
      Str val = key;
      *upsert(perm, &h, key) = val;

      if (create_anim) {
        Arena temp = *perm;
        Pipe pipe = {0};
        {
          Write_Buffer file_path = write_buffer(new(&temp, u8, 128), 128);
          append_str(&file_path, dir_path);
          if      (i < 10)  { append_lit(&file_path, "00"); }
          else if (i < 100) { append_lit(&file_path, "0");  }
          assert(i < 1000);
          append_long(&file_path, i);
          append_lit(&file_path, ".svg");
          append_byte(&file_path, 0);
          pipe = os_start_graphviz((char *)file_path.buf);
        }
        assert(pipe.write_fd > 0);
        Write_Buffer b[1] = { fd_buffer(pipe.write_fd, new(&temp, u8, 8 * 1024), 8 * 1024), };

        append_lit(b, "digraph hash_trie_it {\n");
        append_lit(b, "labelloc=t\n");
        append_lit(b, "label=\"items: "); append_long(b, i + 1); append_lit(b, "\";\n");
        append_lit(b, "nodesep=.05;\n");
        append_lit(b, "node [ shape=box ];\n");
        append_hash_tree(b, h);
        append_lit(b, "}\n");

        flush(b);
        os_stop_graphviz(pipe);
      }
    }

    if (!create_anim) {
      append_lit(stdout, "digraph hash_trie {\n");
      append_lit(stdout, "nodesep=.05;\n");
      append_hash_tree(stdout, h);
      append_lit(stdout, "}\n");
    }
    else {
      os_ffmpeg("trie.mkv");
    }

    flush(stdout);
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//- libc platform

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef __MINGW32__
#define mkdir(p, m) mkdir(p)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#endif

Pipe os_start_graphviz(char *out_file)
{
  int READ_END  = 0;
  int WRITE_END = 1;
  int fd[2];

  if (pipe(fd) < 0) {
    fatal(S("FATAL: Failed to create pipe\n"));
  }

  pid_t child = fork();
  if (child < 0) {
    fatal(S("FATAL: Failed to fork a child\n"));
  }

  if (child == 0) {
    if (dup2(fd[READ_END], STDIN_FILENO) < 0) {
      fatal(S("FATAL: Failed to dup2 pipe as stdin from parent.\n"));
    }
    os_close(fd[WRITE_END]);

    int ret = execlp("dot",
                     "dot",
                     "-T",
                     "svg",
                     "-o",
                     out_file,
                     0);

    if (ret < 0) {
      fatal(S("FATAL: command `dot` failed\n"));
    }

    assert(0 && "unreachable");
    os_exit(1);
  }

  os_close(fd[READ_END]);

  Pipe r = {0};
  r.write_fd = fd[WRITE_END];
  r.pid = child;
  return r;
}

b32 os_stop_graphviz(Pipe pipe)
{
  pid_t pid = pipe.pid;
  int   fd  = pipe.write_fd;

  os_close(fd);

  for (;;) {
    int wstatus = 0;
    if (waitpid(pid, &wstatus, 0) < 0) {
      return 0;
    }

    if (WIFEXITED(wstatus)) {
      int exit_status = WEXITSTATUS(wstatus);
      if (exit_status != 0) {
        return 0;
      }
      return 1;
    }
  }

  return 0;
}

b32 os_ffmpeg(char *out_file)
{
  pid_t child = fork();
  if (child < 0) {
    fatal(S("FATAL: Failed to fork a child\n"));
  }

  if (child == 0) {
    int ret = execlp("ffmpeg",
                     "ffmpeg",
                     "-y",
                     "-framerate", "1/2",
                     "-pattern_type", "glob",
                     "-i", "./out/*.svg",
                     "-s", "1920x480",
                     out_file,
                     0);

    if (ret < 0) {
      fatal(S("FATAL: command `ffmpeg` failed\n"));
    }

    assert(0 && "unreachable");
  }

  for (;;) {
    int wstatus;
    if (waitpid(child, &wstatus, 0) < 0) {
      return 0;
    }
    if (WIFEXITED(wstatus)) {
      int exit_status = WEXITSTATUS(wstatus);
      if (exit_status != 0) {
        return 0;
      }
      return 1;
    }
  }
  return 0;
}


int os_mkdir(char *dir_path)
{
  return mkdir(dir_path, 0755);
}

int os_open(char *file_path)
{
  int r = open(file_path, O_RDWR|O_CREAT, 0655);
  return r;
}

int os_close(int fd)
{
  int ok = close(fd);
  return ok >= 0;
}

b32 os_write(i32 fd, u8 *buf, size len)
{
  for (size off = 0; off < len;) {
    size written = write(fd, buf + off, len - off);
    if (written < 1) { return 0; }
    off += written;
  }
  return 1;
}

void os_exit (int status)
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
