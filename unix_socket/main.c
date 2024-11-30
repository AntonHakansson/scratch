// This is free and unencumbered software released into the public domain.
// anton@hakanssn.com

#include <stdint.h>
#include <stddef.h>
typedef unsigned char Byte;
typedef uint8_t   U8;
typedef int32_t   I32;
typedef int64_t   I64;
typedef uint32_t  U32;
typedef uint64_t  U64;
typedef ptrdiff_t Size;
typedef size_t    USize;
typedef I32    B32;

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define size_of(s) (Size)sizeof(s)
#define align_of(s) (Size)_Alignof(s)
#define count_of(s) (size_of(s) / size_of(*(s)))

#ifndef assert
# define assert(c)  while((!(c))) __builtin_unreachable()
#endif

////////////////////////////////////////////////////////////////////////////////
//- Arena Allocator

#define new(a, t, n) (t *) arena_alloc(a, size_of(t), align_of(t), (n))

typedef struct Arena Arena;
struct Arena {
  Size capacity;
  Size at;
  Byte *backing;
};

static Arena *arena_make(Size capacity)
{
  Arena *a = malloc(sizeof(Arena) + capacity);
  a->backing = (Byte*)a + size_of(*a);
  a->capacity = capacity - size_of(*a);
  a->at = 0;
  return a;
}

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static Byte *arena_alloc(Arena *a, Size objsize, Size align, Size count)
{
  Size avail = a->capacity - a->at;
  Size padding = -(uintptr_t)(a->backing + a->at) & (align - 1);
  Size total   = padding + objsize * count;
  assert(total < avail);
  Byte *p = (a->backing + a->at) + padding;
  memset(p, 0, objsize * count);
  a->at += total;
  return p;
}


////////////////////////////////////////////////////////////////////////////////
//- String slice

#define S(s)        (Str){ .buf = (U8 *)(s), .len = count_of((s)) - 1, }
#define S_FMT(s)    (int)(s).len, (s).buf    /* printf("%.*s", S_FMT(str)) */

typedef struct Str Str;
struct Str {
  U8 *buf;
  Size len;
};


////////////////////////////////////////////////////////////////////////////////
//- Program

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#define TRACE(fmt, ...) printf((fmt), __VA_ARGS__)

int main(int argc, char **argv)
{
  Arena *heap = arena_make(1 << 18);
  Str sock_path = S("/tmp/srv.sock");
  int ok;

  int sock = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    perror("socket");
    exit(1);
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  (void)addr.sun_path; {
    memcpy(addr.sun_path, sock_path.buf, sock_path.len);
    addr.sun_path[sock_path.len] = '\0';
  }

  unlink(addr.sun_path);

  ok = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
  if (ok < 0) {
    perror("bind");
    exit(1);
  }

  ok = listen(sock, 10);
  if (ok < 0) {
    perror("listen");
    exit(1);
  }

  for (;;) {
    Arena temp = *heap;
    struct sockaddr_storage client_addr = {0};
    socklen_t client_addr_len = sizeof client_addr;
    int client = accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client < 0) { perror("accept"); }
    if (client > 0) {
      TRACE("Accepted client %d\n", client);
      Size buffer_capacity = 1 << 12;
      U8 *buffer = new (&temp, U8, buffer_capacity);
      Size n_bytes_read = 0;
      do {
        n_bytes_read = recv(client, buffer, buffer_capacity, 0);
        if (n_bytes_read < 0) {
          perror("read");
          goto close_client;
        }
        else if (n_bytes_read == 0) {
          TRACE("Client closed connection %d\n", client);
        }
        else if (n_bytes_read > 0) {
          printf("Got msg part: \"%.*s\"\n", (int)n_bytes_read, buffer);
        }
      } while (n_bytes_read > 0);
    }

  close_client:
    close(client);
  }

  close(sock);

  return 0;
}

/* Local Variables: */
/* compile-command: "cc main.c -o server -fsanitize=undefined -g3" */
/* End: */
