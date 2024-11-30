// This is free and unencumbered software released into the public domain.
// anton@hakanssn.com

#include <stdint.h>
#include <stddef.h>
typedef char Byte;
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

#define size_of(s)  (Size)sizeof(s)
#define align_of(s) (Size)_Alignof(s)
#define count_of(s) (size_of((s)) / size_of(*(s)))

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

typedef struct Cut Cut;
struct Cut {
  Str head;
  Str tail;
};

static Cut str_cut(Str s, Size i) {
  assert(s.buf);
  assert(i >= 0);
  assert(i <= s.len);
  Cut r = {0};
  r.head.buf = s.buf;
  r.head.len = i;
  r.tail.buf = s.buf + i;
  r.tail.len = s.len - i;
  return r;
}

////////////////////////////////////////////////////////////////////////////////
//- Program

#include <liburing.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <signal.h>

typedef struct State State;
struct State {
  B32 should_close;
  struct io_uring ring;
} state = {0};

typedef struct Request {
  int state;
  struct sockaddr_storage client_addr;
  socklen_t client_addr_len;
  int client_fd;
  U8 *client_request;
} Request;

void queue_accept(Arena *arena, int sock)
{
  Request *req = new(arena, Request, 1); // TODO: pool these in a freelist
  req->state = 1;

  req->client_addr_len = sizeof req->client_addr;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&state.ring);
  io_uring_prep_accept(sqe, sock, (struct sockaddr *)&req->client_addr,
                       &req->client_addr_len, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&state.ring);
}

void queue_read(Arena *arena, Request *req, int client)
{
  Size buf_len = 4096;
  req->client_request = new(arena, U8, buf_len); // TODO pool these buffers
  req->state = 2;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&state.ring);
  io_uring_prep_read(sqe, req->client_fd, req->client_request, buf_len, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&state.ring);
}

void queue_write_(Request *req, Str strs[], Size strs_count)
{
  req->state = 3;

  enum { MAX_STRS = 16 };
  assert(strs_count < 16 && strs_count > 0);
  U64 offset = 0;
  int flags = 0;
  struct iovec iovecs[MAX_STRS] = {0};
  for (int i = 0; i < strs_count; i++) {
    iovecs[i] = (struct iovec){ .iov_base = strs[i].buf, .iov_len = strs[i].len };
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(&state.ring);
  io_uring_prep_writev2(sqe, req->client_fd, iovecs, strs_count, offset, flags);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&state.ring);
}

void queue_write(Request *req, U8 *response, Size len)
{
  req->state = 3;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&state.ring);
  io_uring_prep_write(sqe, req->client_fd, response, len, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&state.ring);
}

void queue_close(Request *req) {
  req->state = 4;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&state.ring);
  io_uring_prep_close(sqe, req->client_fd);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&state.ring);
}

int initialize_socket(Str sock_path)
{
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

  return sock;
}

void sigint_handler(int signo)
{
  printf("^C pressed. Gracefully Shutting down.\n");
  state.should_close = 1; // REVIEW: data race??
}


static Arena *new_arena(Size capacity)
{
  Arena *a = malloc(sizeof(Arena) + capacity);
  a->backing = (Byte*)a + size_of(*a);
  a->capacity = capacity - size_of(*a);
  a->at = 0;
  return a;
}

int main(int argc, char **argv)
{
  Arena *heap = new_arena(1 << 18);
  Str sock_path = S("/tmp/srv.sock");

  signal(SIGINT, sigint_handler);

  int sock = initialize_socket(sock_path);

  io_uring_queue_init(32, &state.ring, 0);
  struct io_uring_cqe *cqe = {0};
  queue_accept(heap, sock);

  while (!state.should_close) {
    int ret = io_uring_wait_cqe(&state.ring, &cqe);
    if (ret < 0) {
      perror("io_uring_wait_cqe");
      continue;
    }

    Request *req = (Request *)cqe->user_data;
    if (cqe->res < 0) {
      fprintf(stderr, "Async request failed: %s for event with state %d\n",
              strerror(-cqe->res), req->state);
    }
    else {
      switch(req->state) {
        case 1: { // accept response
          queue_accept(heap, sock); // replace consumed accept event
          req->client_fd = cqe->res;
          assert(req->client_fd > 0);
          fprintf(stderr, "Accepted client %d\n", req->client_fd);
          queue_read(heap, req, req->client_fd);
        } break;
        case 2: { // read response
          int n_bytes_read = cqe->res;
          assert(n_bytes_read > 0);
          fprintf(stderr, "Read client request with data %.*s\n", n_bytes_read,
                  req->client_request);

          if (1) { // close response
            queue_close(req);
          } else { // write
            Size buf_len = 8;
            U8 *buf = new(heap, U8, buf_len); // TODO pool these buffers
            queue_write(req, buf, buf_len);
          };
        } break;
        case 3: { // write response
          int n_bytes_written = cqe->res;
          assert(n_bytes_written > 0);
          queue_close(req);
        } break;
        case 4: { // close
          // ok
        } break;
        default: { fprintf(stderr, "unknown event\n"); }
      }
    }
    io_uring_cqe_seen(&state.ring, cqe);
  }

  io_uring_queue_exit(&state.ring);

  return 0;
}

/* Local Variables: */
/* compile-command: "nix-shell -p liburing --run 'cc iouring.c -o iouring -luring -fsanitize=undefined -g3'" */
/* End: */
