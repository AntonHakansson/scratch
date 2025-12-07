#if IN_SHELL /* $ bash wayland_over_the_wire.c
 cc wayland_over_the_wire.c -o wayland_over_the_wire -fsanitize=undefined,address -Wall -Wextra -g3 -O3 -march=native -ffast-math
exit # */
#endif

/*
  Wayland over the wire
  ======================

  A program to learn how Wayland protocol works under the hood without third-party dependencies.
  Modestly opens a douple-buffered, resizable window and mandelbrot :s

 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define tassert(c)       while (!(c)) __builtin_trap()
#define breakpoint(c)    ((c) ? ({ asm volatile ("int3; nop"); }) : 0)
#define countof(a)       (ptrdiff_t)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)     ((t *)arena_alloc(a, sizeof(t), _Alignof(t), (n)))
#define newbeg(a, t, n)  ((t *)arena_alloc_beg(a, sizeof(t), _Alignof(t), (n)))
#define s8(s)            (S8){(uint8_t *)s, countof(s)-1}
#define memcpy(d, s, n)  __builtin_memcpy(d, s, n)
#define memset(d, c, n)  __builtin_memset(d, c, n)

////////////////////////////////////////////////////////////////////////////////
//- Arena

typedef struct { uint8_t *beg; uint8_t *end; } Arena;

static uint8_t *arena_alloc(Arena *a, ptrdiff_t objsize, ptrdiff_t align, ptrdiff_t count) {
  ptrdiff_t padding = (size_t)a->end & (align - 1);
  tassert((count <= (a->end - a->beg - padding) / objsize) && "out of memory");
  ptrdiff_t total = objsize * count;
  return memset(a->end -= total + padding, 0, total);
}

static uint8_t *arena_alloc_beg(Arena *a, ptrdiff_t objsize, ptrdiff_t align, ptrdiff_t count) {
  ptrdiff_t padding = -(size_t)(a->beg) & (align - 1);
  ptrdiff_t total   = padding + objsize * count;
  tassert(total < (a->end - a->beg) && "out of memory");
  uint8_t *p = a->beg + padding;
  memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- String

#define s8pri(s) (int)s.len, s.data

typedef struct { uint8_t *data; ptrdiff_t len; } S8;

static S8 s8dup(Arena *a, S8 s) {
  return (S8) {
    memcpy((new(a, uint8_t, s.len)), s.data, s.len * sizeof(uint8_t)),
    s.len,
  };
}

static S8 s8cstr(Arena *a, char *cstr) {
  return s8dup(a, (S8){(uint8_t*)cstr, __builtin_strlen(cstr)});
}

#define s8concat(arena, head, ...)                                                   \
  s8concatv(arena, head, ((S8[]){__VA_ARGS__}), (countof(((S8[]){__VA_ARGS__}))))

static S8 s8concatv(Arena *a, S8 head, S8 *ss, ptrdiff_t count) {
  S8 r = {0};
  if (!head.data || (uint8_t *)(head.data+head.len) != a->beg) {
    S8 copy = head;
    copy.data = newbeg(a, uint8_t, head.len);
    if (head.len) memcpy(copy.data, head.data, head.len);
    head = copy;
  }
  for (ptrdiff_t i = 0; i < count; i++) {
    S8 tail = ss[i];
    uint8_t *data = newbeg(a, uint8_t, tail.len);
    if (tail.len) memcpy(data, tail.data, tail.len);
    head.len += tail.len;
  }
  r = head;
  return r;
}

static _Bool s8match(S8 a, S8 b, ptrdiff_t n) {
  if (a.len < n || b.len < n)  { return 0; }
  for (ptrdiff_t i = 0; i < n; i++) {
    if (a.data[i] != b.data[i]) { return 0; }
  }
  return 1;
}

static _Bool s8equal(S8 a, S8 b) {
  if (a.len != b.len)  { return 0; }
  return s8match(a, b, a.len);
}

static S8 s8i64(Arena *arena, int64_t x) {
  _Bool negative = (x < 0);
  if (negative) { x = -x; }
  char digits[20];
  int i = countof(digits);
  do {
    digits[--i] = (char)(x % 10) + '0';
  } while (x /= 10);
  ptrdiff_t len = countof(digits) - i + negative;
  uint8_t *beg = new(arena, uint8_t, len);
  uint8_t *end = beg;
  if (negative) { *end++ = '-'; }
  do { *end++ = digits[i++]; } while (i < countof(digits));
  return (S8){beg, len};
}

////////////////////////////////////////////////////////////////////////////////
//- Error list

typedef enum Error_Severity {
  Error_Severity_None,
  Error_Severity_Error,
} Error_Severity;

typedef struct Err Err;
struct Err {
  Err *next;
  Error_Severity severity;
  S8 message;
};

typedef struct ErrList {
  Arena arena;
  Arena rewind_arena;
  Arena scratch;
  Err *first;
  Error_Severity max_severity;
} ErrList;

static Err *emit_err(ErrList *errors, Error_Severity severity, S8 message);

static ErrList *errors_make(Arena *arena, ptrdiff_t nbyte) {
  ErrList *r = new(arena, ErrList, 1);
  uint8_t *beg = new(arena, uint8_t, nbyte);
  r->arena = (Arena){beg, beg + nbyte};
  ptrdiff_t scratch_nbyte = 2048;
  uint8_t *scratch_beg = new(&r->arena, uint8_t, scratch_nbyte);
  r->scratch = (Arena){scratch_beg, scratch_beg + scratch_nbyte};
  r->rewind_arena = r->arena;
  return r;
}

static Error_Severity errors_get_max_severity_and_reset(ErrList *errors) {
  Error_Severity max_severity = errors->max_severity;
  errors->first = 0;
  errors->max_severity = Error_Severity_None;
  errors->arena = errors->rewind_arena;
  return max_severity;
}

#define for_errors(errors, varname)                                            \
    for (Err *varname = errors->first; varname && (errors->max_severity > 0);  \
         varname = varname->next)


static Err *emit_err_nondup(ErrList *errors, Error_Severity severity, S8 message) {
  tassert(errors && errors->arena.beg);
  tassert(message.data >= errors->arena.beg && message.data < errors->arena.end);

  _Bool arena_exhausted = (errors->arena.end - errors->arena.beg) < ((ptrdiff_t)sizeof(Err) + message.len);
  if (arena_exhausted) {
    // REVIEW: force flush existing errors to stderr instead?
    errors_get_max_severity_and_reset(errors);
    emit_err(errors, Error_Severity_Error, s8("Exceeded error memory limit. Previous errors omitted."));
  }
  Err *err = new(&errors->arena, Err, 1);
  err->severity = severity;
  err->message = message;
  err->next = errors->first;
  errors->first = err;
  if (severity > errors->max_severity) {
    errors->max_severity = severity;
  }
  return err;
}

static Err *emit_err(ErrList *errors, Error_Severity severity, S8 message) {
  tassert(errors && errors->arena.beg);
  return emit_err_nondup(errors, severity, s8dup(&errors->arena, message));
}

#define emit_errno(errors, ...)                                                \
  do {                                                                         \
    Arena scratch = errors->scratch;                                           \
    S8 msg = s8concat(&scratch, s8(__FILE_NAME__),                             \
                   s8("("), s8i64(&scratch, __LINE__), s8("): "),              \
                   __VA_ARGS__, s8(": "),                                      \
                   s8cstr(&scratch, strerror(errno)));                         \
    emit_err(errors, Error_Severity_Error, msg);                               \
  } while (0);


////////////////////////////////////////////////////////////////////////////////
//- Wayland over the wire

static uint16_t *write_u16(char **at, uint16_t v) {
  uint16_t *r = (uint16_t *)*at;
  *r = v;
  *at += 2;
  return r;
}

static uint32_t *write_u32(char **at, uint32_t v) {
  uint32_t *r = (uint32_t *)*at;
  *r = v;
  *at += 4;
  return r;
}

static char *write_s8_lit_and_align(char **at, S8 lit) {
  tassert(lit.data[lit.len] == 0); // null terminated!
  uint32_t s_len = lit.len + 1;    // include null terminator in length
  write_u32(at, s_len);
  char *r = *at;
  memcpy(r, lit.data, lit.len);
  r[s_len - 1] = 0;
  *at += s_len;
  size_t padding = -(size_t)(*at) & (_Alignof(uint32_t) - 1);
  *at += padding;
  return r;
}

static uint32_t read_u32(char **at) {
  uint32_t r = *(uint32_t *)(*at);
  *at += 4;
  return r;
}

static S8 read_s8_and_align(char **at) {
  uint32_t len = read_u32(at);
  char *cstr = *at;
  *at += len;
  size_t padding = -(size_t)(*at) & (_Alignof(uint32_t) - 1);
  *at += padding;
  return (S8){(uint8_t *)cstr, len > 0 ? len - 1 : 0};
}

enum { MAX_WAYLAND_BUFFER_SIZE = sizeof(int) * 32, };

static int wl_display_get_registry(char *buffer, int buffer_sz, int display, int registry_id) {
  uint16_t message_sz = 4 + 2 + 2 + 4;
  tassert(message_sz <= buffer_sz);

  enum { OPCODE_WL_DISPLAY_GET_REGISTRY = 1, };
  char *at = buffer;
  write_u32(&at, display);
  write_u16(&at, OPCODE_WL_DISPLAY_GET_REGISTRY);
  write_u16(&at, message_sz);
  write_u32(&at, registry_id);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_display_sync(char *buffer, int buffer_sz, int display, int callback_id) {
  uint16_t message_sz = 4 + 2 + 2 + 4;
  tassert(message_sz <= buffer_sz);

  enum { OPCODE_WL_DISPLAY_SYNC = 0, };
  char *at = buffer;
  write_u32(&at, display);
  write_u16(&at, OPCODE_WL_DISPLAY_SYNC);
  write_u16(&at, message_sz);
  write_u32(&at, callback_id);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_registry_bind(char *buffer, int buffer_sz, int registry, uint32_t name, S8 thing, uint32_t required_version, uint32_t bind_id) {
  ptrdiff_t string_size = 4 + thing.len + 1; // length + string + null
  string_size += (-(string_size) & 3); // align to 4 bytes
  ptrdiff_t message_sz = 4 + 2 + 2 + 4 + string_size + 4 + 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_REGISTRY_REQUEST_BIND = 0, };
  char *at = buffer;
  write_u32(&at, registry);
  write_u16(&at, WL_REGISTRY_REQUEST_BIND);
  write_u16(&at, message_sz);
  write_u32(&at, name);
  write_s8_lit_and_align(&at, thing);
  write_u32(&at, required_version);
  write_u32(&at, bind_id);

  tassert(message_sz < buffer_sz);
  return message_sz;
}

static int wl_compositor_create_surface(char *buffer, int buffer_sz, int compositor, int surface_id) {
  uint16_t message_sz = 4 + 2 + 2 + 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_COMPOSITOR_REQUEST_CREATE_SURFACE = 0, };
  char *at = buffer;
  write_u32(&at, compositor);
  write_u16(&at, WL_COMPOSITOR_REQUEST_CREATE_SURFACE);
  write_u16(&at, message_sz);
  write_u32(&at, surface_id);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_surface_commit(char *buffer, int buffer_sz, int surface) {
  uint16_t message_sz = 4 + 2 + 2;
  tassert(message_sz <= buffer_sz);

  enum { WL_SURFACE_REQUEST_COMMIT = 6, };
  char *at = buffer;
  write_u32(&at, surface);
  write_u16(&at, WL_SURFACE_REQUEST_COMMIT);
  write_u16(&at, message_sz);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_surface_attach(char *buffer, int buffer_sz, int surface, int wl_buffer, int xoff, int yoff) {
  uint16_t message_sz = 4 + 2 + 2 + 4 + 4 + 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_SURFACE_REQUEST_ATTACH = 1, };
  char *at = buffer;
  write_u32(&at, surface);
  write_u16(&at, WL_SURFACE_REQUEST_ATTACH);
  write_u16(&at, message_sz);
  write_u32(&at, wl_buffer);
  write_u32(&at, xoff);
  write_u32(&at, yoff);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_surface_damage(char *buffer, int buffer_sz, int surface, int xoff, int yoff, int xdim, int ydim) {
  uint16_t message_sz = 4 + 2 + 2 + 4 * 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_SURFACE_REQUEST_DAMAGE = 2, };
  char *at = buffer;
  write_u32(&at, surface);
  write_u16(&at, WL_SURFACE_REQUEST_DAMAGE);
  write_u16(&at, message_sz);
  write_u32(&at, xoff);
  write_u32(&at, yoff);
  write_u32(&at, xdim);
  write_u32(&at, ydim);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_surface_frame(char *buffer, int buffer_sz, int surface, int callback_id) {
    uint16_t message_sz = 4 + 2 + 2 + 4;
    tassert(message_sz <= buffer_sz);

    enum { WL_SURFACE_REQUEST_FRAME = 3 };
    char *at = buffer;
    write_u32(&at, surface);
    write_u16(&at, WL_SURFACE_REQUEST_FRAME);
    write_u16(&at, message_sz);
    write_u32(&at, callback_id);

    tassert(message_sz == (at - buffer));
    return message_sz;
}

static int wl_shm_create_pool(char *buffer, int buffer_sz, int shm, int shm_pool, int shm_pool_sz) {
  uint16_t message_sz = 4 + 2 + 2 + 4 + 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_SHM_REQUEST_CREATE_POOL = 0, };
  char *at = buffer;
  write_u32(&at, shm);
  write_u16(&at, WL_SHM_REQUEST_CREATE_POOL);
  write_u16(&at, message_sz);
  write_u32(&at, shm_pool);
  write_u32(&at, shm_pool_sz);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_shm_pool_create_buffer(char *buffer, int buffer_sz, int shm_pool, int wl_buffer, int framebuffer_offset, int width, int height, int stride, int format) {
  uint16_t message_sz = 4 + 2 + 2 + 6 * 4;
  tassert(message_sz <= buffer_sz);

  enum { WL_SHM_POOL_REQUEST_CREATE_BUFFER = 0, };
  char *at = buffer;
  write_u32(&at, shm_pool);
  write_u16(&at, WL_SHM_POOL_REQUEST_CREATE_BUFFER);
  write_u16(&at, message_sz);
  write_u32(&at, wl_buffer);
  write_u32(&at, framebuffer_offset);
  write_u32(&at, width);
  write_u32(&at, height);
  write_u32(&at, stride);
  write_u32(&at, format);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_xdg_wm_base_get_xdg_surface(char *buffer, int buffer_sz, int xdg_wm_base, int xdg_surface, int wl_surface) {
  uint16_t message_sz = 4 + 2 + 2 + 4 + 4;
  tassert(message_sz <= buffer_sz);

  enum { XDG_WM_BASE_REQUEST_GET_XDG_SURFACE = 2, };
  char *at = buffer;
  write_u32(&at, xdg_wm_base);
  write_u16(&at, XDG_WM_BASE_REQUEST_GET_XDG_SURFACE);
  write_u16(&at, message_sz);
  write_u32(&at, xdg_surface);
  write_u32(&at, wl_surface);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_xdg_surface_get_toplevel(char *buffer, int buffer_sz, int xdg_surface, int xdg_toplevel) {
  uint16_t message_sz = 4 + 2 + 2 + 4;
  tassert(message_sz <= buffer_sz);

  enum { XDG_SURFACE_REQUEST_GET_TOPLEVEL = 1, };
  char *at = buffer;
  write_u32(&at, xdg_surface);
  write_u16(&at, XDG_SURFACE_REQUEST_GET_TOPLEVEL);
  write_u16(&at, message_sz);
  write_u32(&at, xdg_toplevel);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

static int wl_xdg_surface_ack_configure(char *buffer, int buffer_sz, int xdg_surface, int serial) {
  uint16_t message_sz = 4 + 2 + 2 + 4;
  tassert(message_sz <= buffer_sz);

  enum { XDG_SURFACE_ACK_CONFIGURE = 4, };
  char *at = buffer;
  write_u32(&at, xdg_surface);
  write_u16(&at, XDG_SURFACE_ACK_CONFIGURE);
  write_u16(&at, message_sz);
  write_u32(&at, serial);

  tassert(message_sz == (at - buffer));
  return message_sz;
}

////////////////////////////////////////////////////////////////////////////////
//- Drawing

static void draw_mandelbrot(uint32_t *pixels, uint32_t width, uint32_t height, float zoom, float cx, float cy) {
  for (uint32_t py = 0; py < height; py++) {
    for (uint32_t px = 0; px < width; px++) {
      float x0 = (px - width/2.0)  / zoom + cx;
      float y0 = (py - height/2.0) / zoom + cy;

      float x = 0, y = 0;
      int iter = 0;
      while (x*x + y*y <= 4 && iter < 256) {
        float xtemp = x*x - y*y + x0;
        y = 2*x*y + y0;
        x = xtemp;
        iter++;
      }

      uint8_t r = (uint8_t)(iter * 3 % 256);
      uint8_t g = (uint8_t)(iter * 5 % 256);
      uint8_t b = (uint8_t)(iter * 7 % 256);
      uint32_t color = (r << 16) | (g << 8) | b;
      pixels[py * width + px] = 0xFF000000 | color;
    }
  }
}


////////////////////////////////////////////////////////////////////////////////
//- Program

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/syscall.h> // SYS_memfd_create
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <poll.h>

#include <sys/mman.h> // shm_open
#include <fcntl.h>

#include <time.h>
#include <math.h>

static _Bool socket_send_request(int fd, char *buffer, ptrdiff_t len, ErrList *errors) {
  ptrdiff_t total_written = 0;

  while (total_written < len) {
    ptrdiff_t nbytes_written = write(fd, buffer + total_written, len - total_written);
    if (nbytes_written < 0) {
      emit_errno(errors, s8("write"));
      return 0;
    }
    total_written += nbytes_written;
  }

  return 1;
}

static int unix_socket_connect(S8 socket_path, ErrList *errors) {
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    emit_errno(errors, s8("socket"));
    return sock;
  }

  struct sockaddr_un sock_addr = {0};
  sock_addr.sun_family = AF_UNIX;
  if (socket_path.len + 1 >= countof(sock_addr.sun_path)) {
    close(sock);
    emit_err(errors, Error_Severity_Error,
             s8concat(&errors->scratch, s8("Socket path too long: "),
                      s8i64(&errors->scratch, socket_path.len),
                      s8(" >= "), s8i64(&errors->scratch, sizeof(sock_addr.sun_path))));
    return -1;
  }
  memcpy(sock_addr.sun_path, socket_path.data, socket_path.len);
  sock_addr.sun_path[socket_path.len] = 0;

  if (connect(sock, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) < 0) {
    close(sock);
    emit_errno(errors, s8("connect"));
    return -1;
  }

  return sock;
}

static int create_wayland_shm_pool(int sock, int shm, int shm_pool, int shm_pool_sz, ErrList *errors) {
  int shm_pool_fd = syscall(SYS_memfd_create, "wayland-buffer", 0);
  if (shm_pool_fd < 0) {
    emit_errno(errors, s8("shm_open"));
    return -1;
  }

  if (ftruncate(shm_pool_fd, shm_pool_sz) < 0) {
    emit_errno(errors, s8("ftruncate"));
    close(shm_pool_fd);
    return -1;
  }

  char buffer[MAX_WAYLAND_BUFFER_SIZE];
  ptrdiff_t len = wl_shm_create_pool(buffer, sizeof(buffer), shm, shm_pool, shm_pool_sz);

  char control_memory[CMSG_SPACE(sizeof(int))];

  struct iovec iovec = {
    .iov_base = buffer,
    .iov_len = len
  };

  struct msghdr msg = {
    .msg_iov = &iovec,
    .msg_iovlen = 1,
    .msg_control = control_memory,
    .msg_controllen = sizeof(control_memory)
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &shm_pool_fd, sizeof(int));

  ssize_t nbytes_written = sendmsg(sock, &msg, 0);
  if (nbytes_written < 0) {
    emit_errno(errors, s8("sendmsg"));
    close(shm_pool_fd);
    return -1;
  }

  if (nbytes_written != len) {
    emit_errno(errors, s8("sendmsg incomplete"));
    close(shm_pool_fd);
    return -1;
  }

  return shm_pool_fd;
}

typedef struct __attribute__((packed)) Wl_Header {
  uint32_t object_id;
  uint16_t opcode;
  uint16_t size;
} Wl_Header;

typedef struct Wl_Response_Result Wl_Response_Result;
struct Wl_Response_Result {
  Wl_Header header;
  char   *body;
  ssize_t body_len;
};

static _Bool read_wl_response(Arena *arena, int sock, Wl_Response_Result *out_result, ErrList *errors) {
  Wl_Header header = {0};
  ssize_t nbytes_read = read(sock, &header, sizeof(header));

  if (nbytes_read < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    }
    emit_errno(errors, s8("read"));
    return 0;
  }

  if (nbytes_read == 0) {
    fprintf(stderr, "[INFO] Read EOF from socket. Exiting.");
    return 0;
  }

  if (nbytes_read != sizeof(header)) {
    Arena scratch = errors->scratch;
    emit_err(errors, Error_Severity_Error,
             s8concat(&scratch, s8("Malformed wayland response: Expected "),
                      s8i64(&scratch, sizeof(header)), s8(" bytes, but got "),
                      s8i64(&scratch, nbytes_read)));
    return 0;
  }

  ssize_t remaining = header.size - sizeof(header);
  char *body = new(arena, char, remaining);
  ssize_t body_len = read(sock, body, remaining);

  if (body_len < 0) {
    emit_errno(errors, s8("read"));
    return 0;
  }

  if (body_len != remaining) {
    Arena scratch = errors->scratch;
    emit_err(errors, Error_Severity_Error,
             s8concat(&scratch, s8("Malformed wayland response: Expected "),
                      s8i64(&scratch, remaining),
                      s8(" bytes after header, but got "),
                      s8i64(&scratch, body_len)));
    return 0;
  }

  *out_result = (Wl_Response_Result){
    .header = header,
    .body = body,
    .body_len = body_len,
  };
  return 1;
}

typedef struct Wayland_Framebuffer Wayland_Framebuffer;
struct Wayland_Framebuffer {
  uint32_t id;
  int shm_pool;
  int shm_pool_fd;
  int shm_pool_sz;
  uint32_t width;
  uint32_t height;
  uint32_t *pixels;
  _Bool in_use_by_compositor;
};

typedef struct Wayland Wayland;
struct Wayland {
  uint32_t display;
  uint32_t registry;

  uint32_t shm;
  uint32_t compositor;
  uint32_t xdg_wm_base;

  uint32_t surface;
  uint32_t xdg_surface;
  uint32_t xdg_toplevel;

  uint32_t frame_callback_id;
  _Bool frame_callback_pending;

  Wayland_Framebuffer buffers[2];
  int current_buffer; // currently displayed buffer; 0 or 1
  Wayland_Framebuffer buffers_cleanups[64];
  int buffers_cleanups_count;

  int prev_id;
};

static Wayland_Framebuffer wayland_create_framebuffer(Wayland *wayland, int sock, int width, int height, ErrList *errors) {
    static char buffer[MAX_WAYLAND_BUFFER_SIZE] = {0};
    static const int buffer_capacity = MAX_WAYLAND_BUFFER_SIZE;

    Wayland_Framebuffer result = {0};

    int shm_pool_id = ++wayland->prev_id;
    int shm_pool_sz = height * width * 4;
    int shm_pool_fd = create_wayland_shm_pool(sock, wayland->shm, shm_pool_id, shm_pool_sz, errors);
    if (shm_pool_fd < 0) {
        return result;
    }

    int buffer_id = ++wayland->prev_id;
    enum { WL_SHM_POOL_ENUM_FORMAT_ARGB8888 = 0 };
    int message_sz = wl_shm_pool_create_buffer(buffer, buffer_capacity, shm_pool_id, buffer_id, 0, width, height, width * 4, WL_SHM_POOL_ENUM_FORMAT_ARGB8888);
    if (!socket_send_request(sock, buffer, message_sz, errors)) {
        return result;
    }

    uint32_t *pixels = (uint32_t *)mmap(0, shm_pool_sz, PROT_READ | PROT_WRITE, MAP_SHARED, shm_pool_fd, 0);
    if (pixels == MAP_FAILED) {
        emit_errno(errors, s8("mmap"));
        return result;
    }

    result.id = buffer_id;
    result.shm_pool = shm_pool_id;
    result.shm_pool_fd = shm_pool_fd;
    result.shm_pool_sz = shm_pool_sz;
    result.width = width;
    result.height = height;
    result.pixels = pixels;

    return result;
}

static int wayland_connect(Arena scratch, int sock, ErrList *errors, Wayland *out) {
  static char buffer[MAX_WAYLAND_BUFFER_SIZE] = {0};
  static int buffer_capacity = MAX_WAYLAND_BUFFER_SIZE;
  int message_sz;

  Wayland wayland = {0};
  wayland.display = ++wayland.prev_id;

  wayland.registry = ++wayland.prev_id;
  message_sz = wl_display_get_registry(buffer, buffer_capacity, wayland.display, wayland.registry);
  if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;

  unsigned int registry_creation_done_id = ++wayland.prev_id;
  message_sz = wl_display_sync(buffer, buffer_capacity, wayland.display, registry_creation_done_id);
  if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;

  // Request core Wayland objects
  for (;;) {
    Wl_Response_Result response = {0};
    if (!read_wl_response(&scratch, sock, &response, errors)) return -1;
    if (response.header.object_id == registry_creation_done_id) break;

    enum {
      OPCODE_WL_REGISTRY_EVENT_GLOBAL = 0,

      REQUIRED_WL_SHM_VERSION = 2,
      REQUIRED_COMPOSITOR_VERSION = 6,
      REQUIRED_XDG_WM_BASE_VERSION = 2,
    };

    if (response.header.object_id == wayland.registry && response.header.opcode == OPCODE_WL_REGISTRY_EVENT_GLOBAL) {
      char *at = response.body;
      uint32_t name = read_u32(&at);
      S8 interface = read_s8_and_align(&at);
      uint32_t version = read_u32(&at);

      S8 wl_shm_lit = s8("wl_shm");
      S8 wl_compositor_lit = s8("wl_compositor");
      S8 xdg_wm_base_lit = s8("xdg_wm_base");
      if (s8equal(interface, wl_compositor_lit)) {
        if (version < REQUIRED_COMPOSITOR_VERSION) {
          emit_err(errors, Error_Severity_Error,
                   s8concat(&scratch, s8("Expected wayland compositor version >="),
                            s8i64(&scratch, REQUIRED_COMPOSITOR_VERSION),
                            s8(" but got version "), s8i64(&scratch, version)));
          return -1;
        }
        wayland.compositor = ++wayland.prev_id;
        message_sz = wl_registry_bind(buffer, buffer_capacity, wayland.registry, name, wl_compositor_lit, REQUIRED_COMPOSITOR_VERSION, wayland.compositor);
        if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;
      }
      else if (s8equal(interface, wl_shm_lit)) {
        if (version < REQUIRED_WL_SHM_VERSION) {
          emit_err(errors, Error_Severity_Error,
                   s8concat(&scratch, s8("Expected wayland shm version >="),
                            s8i64(&scratch, REQUIRED_COMPOSITOR_VERSION),
                            s8(" but got version "), s8i64(&scratch, version)));
          return -1;
        }
        wayland.shm = ++wayland.prev_id;
        message_sz = wl_registry_bind(buffer, buffer_capacity, wayland.registry, name, wl_shm_lit, REQUIRED_WL_SHM_VERSION, wayland.shm);
        if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;
      }
      else if (s8equal(interface, xdg_wm_base_lit)) {
        if (version < REQUIRED_XDG_WM_BASE_VERSION) {
          emit_err(errors, Error_Severity_Error,
                   s8concat(&scratch, s8("Expected wayland xdg_wm_base version >="),
                            s8i64(&scratch, REQUIRED_COMPOSITOR_VERSION),
                            s8(" but got version "), s8i64(&scratch, version)));
          return -1;
        }
        wayland.xdg_wm_base = ++wayland.prev_id;
        message_sz = wl_registry_bind(buffer, buffer_capacity, wayland.registry, name, xdg_wm_base_lit, REQUIRED_XDG_WM_BASE_VERSION, wayland.xdg_wm_base);
        if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;
      } else {
        fprintf(stderr, "[DEBUG] Unhandled interface '%.*s'\n", s8pri(interface));
      }
    }
  }
  fprintf(stderr, "[INFO] wl_display = %d, wl_shm = %d, xdg_wm_base = %d\n", wayland.display, wayland.shm, wayland.xdg_wm_base);

  wayland.surface = ++wayland.prev_id;
  message_sz = wl_compositor_create_surface(buffer, buffer_capacity, wayland.compositor, wayland.surface);
  if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;

  wayland.xdg_surface = ++wayland.prev_id;
  message_sz = wl_xdg_wm_base_get_xdg_surface(buffer, buffer_capacity, wayland.xdg_wm_base, wayland.xdg_surface, wayland.surface);
  if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;

  wayland.xdg_toplevel = ++wayland.prev_id;
  message_sz = wl_xdg_surface_get_toplevel(buffer, buffer_capacity, wayland.xdg_surface, wayland.xdg_toplevel);
  if (!socket_send_request(sock, buffer, message_sz, errors)) return -1;

  for (ptrdiff_t i = 0; i < countof(wayland.buffers); i++) {
    enum { DEFAULT_WIDTH = 640, DEFAULT_HEIGHT = 480 };
    wayland.buffers[i] = wayland_create_framebuffer(&wayland, sock, DEFAULT_WIDTH, DEFAULT_HEIGHT, errors);
    if (wayland.buffers[i].pixels == 0) return -1;
  }

  { // Make socket non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
      emit_errno(errors, s8("fcntl F_GETFL"));
      return -1;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
      emit_errno(errors, s8("fcntl F_SETFL O_NONBLOCK"));
      return -1;
    }
  }

  *out = wayland;
  return 0;
}

static void wayland_swap_buffers(Wayland *wayland) {
  int back_buffer = 1 - wayland->current_buffer;
  wayland->buffers[1 - wayland->current_buffer].in_use_by_compositor = 1;
  wayland->current_buffer = back_buffer;
}

static S8 getenv_wrapped(const char *name) {
  char *s = getenv(name);
  if (!s) return (S8){0};
  return (S8) {(uint8_t*)s, strlen(s)};
}

static Arena os_make_arena(ptrdiff_t nbyte) {
  uint8_t *heap = malloc(nbyte);
  return (Arena){heap, heap + nbyte};
}

int main() {
  Arena arena[1] = { os_make_arena(1u<<16) };

  const Arena scratch_rewind = os_make_arena(1u<<16);
  Arena scratch = scratch_rewind;

  ErrList *errors = errors_make(arena, 1 << 12);

  int sock = 0;
  {
    S8 socket_path = s8concat(&scratch, getenv_wrapped("XDG_RUNTIME_DIR"), s8("/"));
    socket_path = s8concat(&scratch, socket_path, getenv_wrapped("WAYLAND_DISPLAY"));
    sock = unix_socket_connect(socket_path, errors);
    if (sock < 0) goto err;
  }

  Wayland wayland = {0};
  if (wayland_connect(scratch, sock, errors, &wayland) < 0) goto err;

  static char buffer[MAX_WAYLAND_BUFFER_SIZE] = {0};
  static const int buffer_capacity = MAX_WAYLAND_BUFFER_SIZE;
  int message_sz = 0;

  _Bool running = 1;
  while (running) {
    scratch = scratch_rewind;
    Wayland_Framebuffer *draw_buffer = &wayland.buffers[1 - wayland.current_buffer];

    struct pollfd pfd = {.fd = sock, .events = POLLIN};
    int ret = poll(&pfd, 1, 16); // 16ms timeout
    if (ret < 0) { emit_errno(errors, s8("poll")); goto err; }
    if (!(pfd.revents & POLLIN)) { continue; /* No events */ }

    // Process pending Wayland events
    while (running) {
      Wl_Response_Result response = {0};
      if (!read_wl_response(&scratch, sock, &response, errors)) goto err;
      if (response.header.size == 0) break; // Empty

      _Bool found_response_route = 0;

      if (response.header.object_id == wayland.xdg_surface) {
        found_response_route = 1;
        switch(response.header.opcode) {
        case 0: {
          char *at = response.body;
          uint32_t serial = read_u32(&at);

          message_sz = wl_xdg_surface_ack_configure(buffer, buffer_capacity, wayland.xdg_surface, serial);
          if (!socket_send_request(sock, buffer, message_sz, errors)) goto err;
        } break;
        default: {
          fprintf(stderr, "[INFO] Unhandled opcode %d", response.header.opcode);
        } break;
        }
      } else if (response.header.object_id == wayland.xdg_toplevel) {
        found_response_route = 1;
        char *at = response.body;

        switch (response.header.opcode) {
        case 0: {
          uint32_t width = read_u32(&at);
          uint32_t height = read_u32(&at);
          uint32_t states_len = read_u32(&at);
          tassert((at - response.body) + states_len < response.header.size);
          uint8_t *states = new(&scratch, uint8_t, states_len);
          for (uint32_t i = 0; i < states_len; i++) {
            states[i] = *at++;
          }

          // Debug print
          {
            fprintf(stderr, "[INFO] xdg_toplevel:configure(%d, %d, %d,", width, height, states_len);
            for (uint32_t i = 0; i < states_len; i++) {
              if (i) fprintf(stderr, ", ");
              fprintf(stderr, "%d", states[i]);
            }
            fprintf(stderr, ")\n");
          }

          _Bool should_resize_framebuffer = 0;
          should_resize_framebuffer |= (draw_buffer->width  != width);
          should_resize_framebuffer |= (draw_buffer->height != height);
          if (should_resize_framebuffer) {
            for (ptrdiff_t i = 0; i < countof(wayland.buffers); i++) {
              wayland.buffers_cleanups[wayland.buffers_cleanups_count++] = wayland.buffers[i];
              wayland.buffers[i] = wayland_create_framebuffer(&wayland, sock, width, height, errors);
            }
          }
        } break;
        case 1: {
          running = 0;
          fprintf(stderr, "[INFO] xdg_toplevel:close()\n");
        } break;
        case 2: {
          fprintf(stderr, "[INFO] xdg_toplevel:configure_bounds()\n");
        } break;
        case 3: {
          fprintf(stderr, "[INFO] xdg_toplevel:wm_capabilities()\n");
        } break;
        default: {
          fprintf(stderr, "[INFO] xdg_toplevel:(unhandled opcode %d)\n", response.header.opcode);
        } break;
        }
      }
      else if (response.header.object_id == wayland.display) {
        found_response_route = 1;
        char *at = response.body;
        switch (response.header.opcode) {
        case 0: {
          uint32_t object_id = read_u32(&at);
          uint32_t error_code = read_u32(&at);
          S8 error_message = read_s8_and_align(&at);
          fprintf(stderr, "[INFO] wl_display:error(%d, %d, %.*s)\n", object_id, error_code, s8pri(error_message));
        } break;
        case 1: {
          uint32_t object_id = read_u32(&at);
          if (0) fprintf(stderr, "[INFO] wl_display:delete_id(%d)\n", object_id);
        } break;
        default: {
          fprintf(stderr, "[INFO] wl_display:(unhandled opcode %d)\n", response.header.opcode);
        } break;
        }
      }

      // Handle framebuffer events
      for (int buffer_idx = 0; buffer_idx < countof(wayland.buffers); buffer_idx++) {
        if (response.header.object_id == wayland.buffers[buffer_idx].id) {
          found_response_route = 1;
          switch (response.header.opcode) {
          case 0: { // wl_buffer:release()
            wayland.buffers[buffer_idx].in_use_by_compositor = 0;
          } break;
          default: {
            fprintf(stderr, "[INFO] wl_buffer:(unhandled opcode %d)\n", response.header.opcode);
          } break;
          }
        }
      }

      // Release resources of old framebuffers no longer used by compositor
      for (int buffers_cleanup_idx = wayland.buffers_cleanups_count - 1; buffers_cleanup_idx >= 0; buffers_cleanup_idx--) {
        Wayland_Framebuffer *buffer = &wayland.buffers_cleanups[buffers_cleanup_idx];
        if (!buffer->in_use_by_compositor) {
          munmap(buffer->pixels, buffer->shm_pool_sz);
          close(buffer->shm_pool_fd);
          wayland.buffers_cleanups[buffers_cleanup_idx] = wayland.buffers_cleanups[--wayland.buffers_cleanups_count];
        }
        if (response.header.object_id == buffer->id) {
          found_response_route = 1;
          switch (response.header.opcode) {
          case 0: { // wl_buffer:release()
            munmap(buffer->pixels, buffer->shm_pool_sz);
            close(buffer->shm_pool_fd);
            wayland.buffers_cleanups[buffers_cleanup_idx] = wayland.buffers_cleanups[--wayland.buffers_cleanups_count];
          } break;
          }
        }
      }

      // Compositor signals that new frame is suitable now
      if (response.header.object_id == wayland.frame_callback_id) {
        found_response_route = 1;
        switch (response.header.opcode) {
        case 0: { // wl_callback:done
          char *at = response.body;
          uint32_t current_time_ms = read_u32(&at);
          (void)current_time_ms; // 𝚫7ms on my machine
          wayland.frame_callback_pending = 0;
        } break;
        }
      }

      if (!found_response_route) {
        fprintf(stderr, "[INFO] unhandled event for object_id %d, opcode %d\n", response.header.object_id, response.header.opcode);
      }
    }

    if (draw_buffer->in_use_by_compositor) continue;
    if (wayland.frame_callback_pending)    continue;

    // Clear pixels
    for (uint32_t y = 0; y < draw_buffer->height; y++) {
      for (uint32_t x = 0; x < draw_buffer->width; x++) {
        draw_buffer->pixels[y * draw_buffer->width + x] = 0xFF000000;
      }
    }

    // Draw
    static float zoom = 90;
    zoom *= 1.01;
    draw_mandelbrot(draw_buffer->pixels, draw_buffer->width, draw_buffer->height, zoom, -0.72, -0.20);

    // Request next frame
    wayland.frame_callback_id = ++wayland.prev_id;
    message_sz = wl_surface_frame(buffer, buffer_capacity, wayland.surface, wayland.frame_callback_id);
    if (!socket_send_request(sock, buffer, message_sz, errors)) goto err;
    wayland.frame_callback_pending = 1;

    // Attach, damage, commit
    message_sz = wl_surface_attach(buffer, buffer_capacity, wayland.surface, draw_buffer->id, 0, 0);
    if (!socket_send_request(sock, buffer, message_sz, errors)) goto err;
    message_sz = wl_surface_damage(buffer, buffer_capacity, wayland.surface, 0, 0, draw_buffer->width, draw_buffer->height);
    if (!socket_send_request(sock, buffer, message_sz, errors)) goto err;
    message_sz = wl_surface_commit(buffer, buffer_capacity, wayland.surface);
    if (!socket_send_request(sock, buffer, message_sz, errors)) goto err;

    wayland_swap_buffers(&wayland);
  }

  close(sock);

 err:
  for_errors(errors, error) {
    fprintf(stderr, "[ERROR] %.*s", s8pri(error->message));
  }
  return (int)errors_get_max_severity_and_reset(errors);
}
