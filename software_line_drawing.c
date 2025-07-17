#if IN_SHELL /* $ bash software_line_drawing.c
cc software_line_drawing.c -o software_line_drawing    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc software_line_drawing.c -o software_line_drawing.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib -Wno-unused-function
# cc software_line_drawing.c -o software_line_drawing    -DAMALGAMATION -Wall -O3 -lpthread -lraylib
exit # */
#endif

// title: Deriving/Exploring Bresenham line drawing algorithm.
// license: This is free and unencumbered software released into the public domain.

#include <stdio.h>
#include <stdlib.h>
#include <raylib.h>
#include <raymath.h>

typedef unsigned char U8;
typedef unsigned int       U32;
typedef unsigned long long U64;
typedef          long long I64;
typedef typeof((char *)0-(char *)0) Iz;
typedef typeof(sizeof(0))           Uz;

#define size_of(s)   (Iz)sizeof(s)
#define count_of(s)  (size_of((s)) / size_of(*(s)))
#define assert(c)    while((!(c))) __builtin_trap()
#define new(a, t, n) ((t *) arena_alloc(a, size_of(t), (Iz)_Alignof(t), (n)))
#define memcpy(d, s, n)  __builtin_memcpy(d, s, n)
#define min(a, b) (((a) < (b)) ? (a) : (b))

typedef struct { U8 *beg, *end; } Arena;

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Iz objsize, Iz align, Iz count) {
  Iz padding = -(Uz)(a->beg) & (align - 1);
  Iz total   = padding + objsize * count;
  if (total >= (a->end - a->beg)) {
		TraceLog(LOG_FATAL, "Out of memory.");
  }
  U8 *p = a->beg + padding;
  __builtin_memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- Executable <-> App interface

typedef struct {
  Arena *perm;
  Arena *frame;
  Arena *read_only;
} App_Update_Params;

////////////////////////////////////////////////////////////////////////////////
//- Executable / Event loop

#if !defined(BUILD_RELOADABLE)

#if defined(AMALGAMATION)
	void *update(Arena *, Arena *, void *);
#else
  extern void perror(const char *s);

  #define RTLD_NOW 0x2
	extern void *dlopen  (const char *__file, int __mode);
	extern int   dlclose (void *__handle);
	extern void *dlsym   (void *__restrict __handle, const char *__restrict __name);
#endif // AMALGAMATION

typedef struct {
  void *handle;
  void *(*update)(App_Update_Params, void *);
  long last_modified;
} AppCode;

static AppCode maybe_load_or_reload_app_code(AppCode app_code, _Bool should_reload) {
  AppCode result = app_code;
  _Bool should_init = (app_code.handle == 0);

  #if !defined(AMALGAMATION)
  long modtime = GetFileModTime("./software_line_drawing.so");
  if (app_code.handle) {
    if (app_code.last_modified != modtime) {
      should_reload |= 1;
    }
  }
  #endif

	if (should_reload) {
   #if defined(AMALGAMATION)
   #else
			assert(app_code.handle && "Can't reload unloaded executable.");
			void *dummy = app_code.handle;
			app_code.update((App_Update_Params){}, dummy); // nofify pre-reload
			dlclose(app_code.handle);
   #endif
  }
  if (should_init || should_reload) {
    result.handle = 0;
    result.update = 0;
		#if defined(AMALGAMATION)
				result.handle = update;
				result.update = update;
    #else
		  result.handle = dlopen("./software_line_drawing.so", RTLD_NOW);
      if (result.handle) {
        result.update = dlsym(result.handle, "update");
		    assert(result.update);
        result.last_modified = modtime;
      }
      else {
        perror("dlopen");
      }
    #endif
  }

  return result;
}

int main(int argc, char **argv) {
  Iz HEAP_CAP = 1ll << 30;
  void *heap = MemAlloc(HEAP_CAP);

	Arena arena = (Arena){heap, heap + HEAP_CAP};
  Arena frame = {0};
  {
	  Iz frame_cap = 1ll << 26;
    frame.beg = new(&arena, U8, frame_cap);
		frame.end = frame.beg + frame_cap;
  }

	AppCode app_code = {0};
	void *app_state = 0;

  SetConfigFlags(FLAG_MSAA_4X_HINT);
	InitWindow(800,600, "Software_Line_Drawing");
	SetWindowState(FLAG_WINDOW_RESIZABLE);
  while (!WindowShouldClose()) {
    Arena frame_reset = frame;
    app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
    if (app_code.update) {
      app_state = app_code.update(
          (App_Update_Params){
              .perm = &arena,
              .frame = &frame,
          }, app_state);
    }
    frame = frame_reset;
	}
	CloseWindow();

  MemFree(heap);
  return 0;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//- App Code

#if defined(AMALGAMATION) || defined(BUILD_RELOADABLE)

#include "ffmpeg_linux.c"

#define MAX_STATE_CAP (1 << 12)

typedef struct {
  Iz struct_size;

  Arena *perm;
  Arena *frame;

  Camera2D camera;
} State;

State *p = 0;

typedef struct IVec2 {
  U32 x, y;
} IVec2;

typedef struct IVec2s {
  IVec2 *items;
  Iz count;
  Iz capacity;
} IVec2s;

//{generate dynamic_array IVec2s IVec2
static void ivec2s_grow(Arena *a, IVec2s *slice) {
  slice->capacity += !slice->capacity;
  if (a->beg == (U8*)(slice->items + slice->count)) {
    new(a, IVec2, slice->capacity);
    slice->capacity *= 2;
  }
  else {
    IVec2 *data = new(a, IVec2, slice->capacity * 2);
    if (slice->count)
      memcpy(data, slice->items, sizeof(IVec2) * slice->count);
    slice->items = data;
    slice->capacity *= 2;
  }
}

static IVec2 *ivec2s_push(Arena *a, IVec2s *slice) {
  if (slice->count >= slice->capacity)
    ivec2s_grow(a, slice);
  return slice->items + slice->count++;
}
//}

static IVec2s ivec2s_reserve(Arena *a, Iz capacity) {
  assert(capacity > 0);
  IVec2s r = {};
  r.items = new(a, IVec2, capacity);
  r.capacity = capacity;
  return r;
}

// Assuming positive slope, dx > dy, and x1 < x2
static IVec2s line_float_naive(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};
  int dx = x2 - x1;
  int dy = y2 - y1;
  float slope = (float)dy / (float)dx;
  float y = y1;
  for (; x1 <= x2; x1++, y += slope) {
    y1 = (int)round(y);
    *(ivec2s_push(a, &r)) = (IVec2){.x = x1, .y = y1,};
  }
  return r;
}

// First step to move to integer arithmetic is to replace rounding
// operation with a decision variable. The decision variable dictates
// if next pixel should advance y coordinate. Let the decision
// variable be the distance to the midpoint pixel of mathematical line
// and top/bottom pixel.

static IVec2s line_float_noround(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};
  int dx = x2 - x1;
  int dy = y2 - y1;
  float slope = (float)dy / (float)dx;
  float err = 0;
  for (int x = x1; x <= x2; x++) {
    *(ivec2s_push(a, &r)) = (IVec2){.x = x, .y = y1,};
    if (err + slope < 0.5) {
      err = err + slope;
    } else {
      // Pixel crossed midpoint of pixel!
      y1++;
      err = err + slope - 1;
    }
  }
  return r;
}

// Now consider decision statement if (err + slope < 0.5). In order to
// move to integer arithmetic we multiply by 2 * dx:

// if ( err + slope < 0.5 )
// if ( err*dx + dy < 0.5 dx )     times dx
// if ( 2*err*dx + 2*dy < dx )     times 2
// if ( 2(err*dx + dy) < dx )      no floating point except for err variable

// Now, what happens if we rewrite algorithm with substitute e' for
// err * dx i.e. decision statement becomes:
//     if ( 2(e' + dy) < dx )

// err      = err      + slope
// err * dx = err * dx + dy
// e' = e' + dy

// err      = err      + slope - 1
// err * dx = err * dx + dy    - dx
// e' = e' + dy - dx

static IVec2s line_float_err_times_dx_times_2(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};
  int dx = x2 - x1;
  int dy = y2 - y1;
  if (dx < 0) return r;
  if (dy < 0) return r;
  float err_prime = 0;
  for (int x = x1; x <= x2; x++) {
    *(ivec2s_push(a, &r)) = (IVec2){.x = x, .y = y1,};
    if (2*(err_prime + dy) < dx) {
      err_prime = err_prime + dy;
    } else {
      y1++;
      err_prime = err_prime + dy - dx;
    }
  }
  return r;
}

// Note that err_prime can now be integer!!! And we have succesfully
// derived bresenham line drawing algorithm. Let's do a final pass and
// eliminate superfluous state and computation.

static IVec2s line_integer(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};
  int dx = x2 - x1;
  int dy = y2 - y1;
  if (dx < 0) return r;
  if (dy < 0) return r;
  int eps = 0;
  for (; x1 <= x2; x1++) {
    *(ivec2s_push(a, &r)) = (IVec2){.x = x1, .y = y1,};
    eps += dy;
    if (eps * 2 >= dx) {
      y1++; eps -= dx;
    }
  }
  return r;
}

// A full implementation should handle positive and negative slope, dy
// > dx, x2 > x1, and y2 > y1

static IVec2s line_bresenham_horiz(Arena *a, int x1, int y1, int x2, int y2) {
  assert(x2 >= x1);
  int dx = x2 - x1;
  int dy = y2 - y1;
  int sy = dy < 0 ? -1 : 1;
  int D = 2*dy*sy - dx;
  IVec2s r = ivec2s_reserve(a, dx + 1);
  for (; x1 <= x2; x1++) {
    *(ivec2s_push(a, &r)) = (IVec2){ .x = x1, .y = y1, };
    if (D > 0) {
      y1 += sy;
      D -= 2*dx;
    }
    D += 2*dy*sy;
  }
  return r;
}
static IVec2s line_bresenham_vert(Arena *a, int x1, int y1, int x2, int y2) {
  assert(y2 >= y1);
  int dx = x2 - x1;
  int dy = y2 - y1;
  int sx = dx < 0 ? -1 : 1;
  int D = 2*dx*sx - dy;
  IVec2s r = ivec2s_reserve(a, dy + 1);
  for (; y1 <= y2; y1++) {
    *(ivec2s_push(a, &r)) = (IVec2){ .x = x1, .y = y1, };
    if (D > 0) {
      x1 += sx;
      D -= 2*dy;
    }
    D += 2*dx*sx;
  }
  return r;
}

static IVec2s line_bresenham(Arena *a, int x1, int y1, int x2, int y2) {
  float dx = x2 - x1;
  float dy = y2 - y1;

  float dx_abs = dx;
  float dy_abs = dy;
  if (dx_abs < 0) dx_abs = -dx;
  if (dy_abs < 0) dy_abs = -dy;

  if (dx_abs > dy_abs) {
    if (dx > 0) return line_bresenham_horiz(a, x1, y1, x2, y2);
    else        return line_bresenham_horiz(a, x2, y2, x1, y1);
  }
  else {
    if (dy > 0) return line_bresenham_vert(a, x1, y1, x2, y2);
    else        return line_bresenham_vert(a, x2, y2, x1, y1);
  }
}


static IVec2s line_float(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};

  int dx = x2 - x1;
  int dy = y2 - y1;

  if (abs(dy) < abs(dx)) {
    int sx = dx < 0 ? -1 : 1;
    float slope = (float)dy / (float)dx;
    float y = y1;
    for (; x1 != x2 + sx; x1 += sx) {
      y1 = (int)round(y);
      *(ivec2s_push(a, &r)) = (IVec2){.x = x1, .y = y1,};
      y += slope * sx;
    }
  } else {
    int sy = dy < 0 ? -1 : 1;
    float slope = (float)dx / (float)dy;
    float x = x1;
    for (; y1 != y2 + sy; y1 += sy) {
      x1 = (int)round(x);
      *(ivec2s_push(a, &r)) = (IVec2){.x = x1, .y = y1,};
      x += slope * sy;
    }
  }
  return r;
}


typedef struct {
  int dim;
  int cell_dim;
  int x, y; 
} Grid;

enum GRID_MAPPING_FLAGS {
  GRID_MAPPING_CLAMP_X = (1l << 0),
  GRID_MAPPING_CLAMP_Y = (1l << 1),
  GRID_MAPPING_CLAMP = GRID_MAPPING_CLAMP_X | GRID_MAPPING_CLAMP_Y,
  GRID_MAPPING_WARP_X = (1l << 2),
  GRID_MAPPING_WARP_Y = (1l << 3),
  GRID_MAPPING_WARP = GRID_MAPPING_WARP_X | GRID_MAPPING_WARP_Y,
};

static IVec2 grid_world_to_cell(Grid g, int x, int y, U32 flags) {
  IVec2 r = {0};
  r.x = (x - g.x) / g.cell_dim;
  r.y = (y - g.y) / g.cell_dim;
  if (flags & GRID_MAPPING_CLAMP_X) {
    if (r.x < 0)     r.x = 0;
    if (r.x > g.dim) r.x = g.dim;
  }
  if (flags & GRID_MAPPING_CLAMP_Y) {
    if (r.y < 0)     r.y = 0;
    if (r.y > g.dim) r.y = g.dim;
  }
  if (flags & GRID_MAPPING_WARP_X) {
    while (r.x < 0) r.x += g.dim;
    while (r.x > g.dim) r.x -= g.dim;
  }
  if (flags & GRID_MAPPING_WARP_Y) {
    while (r.y < 0) r.y += g.dim;
    while (r.y > g.dim) r.y -= g.dim;
  }
  return r;
}

static IVec2 grid_cell_to_world(Grid g, int x, int y, U32 flags) {
  IVec2 r = {0};
  if (flags & GRID_MAPPING_CLAMP_X) {
    if (x > g.dim) x = g.dim;
    if (x < 0) x = 0;
  }
  if (flags & GRID_MAPPING_CLAMP_Y) {
    if (y > g.dim) y = g.dim;
    if (y < 0) y = 0;
  }
  if (flags & GRID_MAPPING_WARP_X) {
    while (x < 0) x += g.dim;
    while (x > g.dim) x -= g.dim;
  }
  if (flags & GRID_MAPPING_WARP_Y) {
    while (y < 0) y += g.dim;
    while (y > g.dim) y -= g.dim;
  }
  r.x = (x * g.cell_dim) + g.x;
  r.y = (y * g.cell_dim) + g.y;
  return r;
}

static void grid_draw_pixels(Grid g, IVec2s ps, U32 pixel_value) {
  for (Iz i = 0; i < ps.count; i++) {
    IVec2 p = ps.items[i];
    IVec2 world = grid_cell_to_world(g, p.x, p.y, GRID_MAPPING_CLAMP);
    Color color;
    memcpy(&color, &pixel_value, sizeof pixel_value);
    DrawRectangle(world.x - g.cell_dim / 2,
                  world.y - g.cell_dim / 2,
                  g.cell_dim,
                  g.cell_dim,
                  color);
  }
}


static Camera2D camera_update(Camera2D camera, _Bool *mouse_input_consumed) {
  _Bool dummy;
  if (camera.zoom == 0.f) { camera.zoom = 1.0f; }
  if (mouse_input_consumed == 0) { mouse_input_consumed = &dummy; }

  Camera2D result = camera;

  if (IsKeyDown(KEY_LEFT_ALT) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    Vector2 delta = GetMouseDelta();
    delta = Vector2Scale(delta, -1.0f/camera.zoom);
    result.target = Vector2Add(camera.target, delta);
    *mouse_input_consumed = 1;
  }

   // Zoom based on mouse wheel
   float wheel = GetMouseWheelMove();
   if (wheel != 0) {
     // Offset and target
     Vector2 mouse_world_pos = GetScreenToWorld2D(GetMousePosition(), camera);
     result.offset = GetMousePosition();
     result.target = mouse_world_pos;

     // Zoom increment
     float scale_factor = 1.0f + (0.25f*fabsf(wheel));
     if (wheel < 0) scale_factor = 1.0f/scale_factor;
     result.zoom = Clamp(camera.zoom * scale_factor, 0.125f, 64.0f);

     *mouse_input_consumed = 1;
   }

  return result;
}


void *update(App_Update_Params params, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(params.perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = params.perm;
    p->frame = params.frame;

    p->camera.zoom = 1.f;
  }
  if (params.perm == 0 && params.frame == 0) { // Pre-reload
    TraceLog(LOG_INFO, "Reload.");
    return p;
  }
  if (p == 0) {    // Post-reload
    State *prev_p = (State *)pstate;
    if (prev_p->struct_size != size_of(*p)) {
      TraceLog(LOG_INFO, "Resized State schema %ld -> %ld", prev_p->struct_size, size_of(*p));
    }
    p = prev_p;
    p->struct_size = size_of(*p);
  }
  assert(p);

  BeginDrawing();
  ClearBackground(LIGHTGRAY);

  _Bool mouse_consumed = 0;
  p->camera = camera_update(p->camera, &mouse_consumed);
  BeginMode2D(p->camera);

  Grid grid = {0};
  grid.cell_dim = 16;
  grid.y = 100;
  grid.dim = min(GetRenderHeight() - grid.y, GetRenderWidth() - grid.x) / grid.cell_dim;

  static _Bool playing = 1;
  if (IsKeyPressed(KEY_SPACE)) { playing = !playing; }

  static int visualization = 0;
  int visualizations_count = 2;
  visualization += IsKeyPressed(KEY_RIGHT);
  visualization -= IsKeyPressed(KEY_LEFT);
  if (visualization < 0) visualization = visualizations_count - 1;
  if (visualization >= visualizations_count) visualization = 0;

  static double anim_time = 0.f;
  if (playing) { anim_time += GetFrameTime(); }

  IVec2 p1 = (IVec2){grid.dim / 2, grid.dim / 2};
  IVec2 p2 = {0};
  p2.x = p1.x + cos(anim_time) * grid.dim / 2;
  p2.y = p1.y + sin(anim_time) * grid.dim / 2;

  // Integer line drawing p1, p2
  {
    int yoffset = (visualization == 1) ? 2 : 0;
    grid_draw_pixels(grid, line_float(p->frame, p1.x, p1.y, p2.x, p2.y), 0xFFFF0000);
    grid_draw_pixels(grid, line_bresenham(p->frame, p1.x, p1.y + yoffset, p2.x, p2.y + yoffset), 0xFF0000FF);
  }

  // Render high resolution line p1, p2
  {
    IVec2 wp1 = grid_cell_to_world(grid, p1.x, p1.y, GRID_MAPPING_CLAMP);
    IVec2 wp2 = grid_cell_to_world(grid, p2.x, p2.y, GRID_MAPPING_CLAMP);
    DrawLine(wp1.x, wp1.y, wp2.x, wp2.y, GREEN);
    DrawCircle(wp1.x, wp1.y, 8, GREEN);
    DrawCircle(wp2.x, wp2.y, 8, GREEN);
  }

  // Render grid
  {
    for (Iz y = 0; y <= grid.dim; y++) {
      IVec2 wp = grid_cell_to_world(grid, 0, y, GRID_MAPPING_CLAMP);
      DrawLine(wp.x - grid.cell_dim / 2, wp.y - grid.cell_dim / 2,
               wp.x - grid.cell_dim / 2 + grid.dim * grid.cell_dim, wp.y - grid.cell_dim / 2, GRAY);
    }
    for (Iz x = 0; x <= grid.dim; x++) {
      IVec2 wp = grid_cell_to_world(grid, x, 0, GRID_MAPPING_CLAMP);
      DrawLine(wp.x - grid.cell_dim / 2, wp.y - grid.cell_dim / 2,
               wp.x - grid.cell_dim / 2, wp.y - grid.cell_dim / 2 + grid.dim * grid.cell_dim, GRAY);
    }
  }

  // Render legend
  {
    int y_cursor = 0;
    int legend_w = 18;
    DrawRectangle(grid.x, y_cursor, legend_w, legend_w, RED);
    DrawText("Bresenham line", grid.x + legend_w + 10,
             y_cursor + legend_w / 2 - 12 / 2, 12, BLACK);
    y_cursor += legend_w + 10;
    DrawRectangle(grid.x, y_cursor, legend_w, legend_w, BLUE);
    DrawText("Floating point approx", grid.x + legend_w + 10,
             y_cursor + legend_w / 2 - 12 / 2, 12, BLACK);
    y_cursor += legend_w + 10;

    DrawText("Press SPACE to stop/play line animation.", grid.x, y_cursor, 12,
             BLACK);
    y_cursor += legend_w;
    DrawText("Press LEFT / RIGHT to cycle between visualizations.", grid.x, y_cursor, 12,
             BLACK);
  }

  EndMode2D();
  EndDrawing();

  {
    static FFMPEG *ffmpeg = 0;
    static U8 is_recording = 0;
    if (IsKeyPressed(KEY_P)) {
      is_recording = !is_recording;
      TraceLog(LOG_INFO, "Recording %s", is_recording ? "on" : "off");
      if (is_recording) {
        ffmpeg = ffmpeg_start_rendering(GetRenderWidth(), GetRenderHeight(), 60);
      }
      else {
        ffmpeg_end_rendering(ffmpeg);
        ffmpeg = 0;
      }
    }
    if (is_recording) {
      Image screenshot = LoadImageFromScreen();
      ffmpeg_send_frame(ffmpeg, screenshot.data, screenshot.width, screenshot.height);
      UnloadImage(screenshot);
    }
  }

  return p;
}

// APPENDIX

static IVec2s line_bresenham_horiz_positive_slope_slow(Arena *a, int x1, int y1, int x2, int y2) {
  // (implicit) line equation   f(x, y) := (x2 - x1)(y - y1) - (y2 - y1)(x - x1) = 0
  // substitute in dx, dy gives f(x, y) := (y - y1)dx - (x - x1)dy = 0

  // Note the following characteristic of function gradient:
  // -  For (x, y) above exact line f(x, y) > 0
  // -  Fox (x, y) below exact line f(x, y) < 0
  //
  // We use this to represent current deviation (in units of dx and dy) from exact curve:
  //     error = f(x, y)

  // Say we already filled pixel (x,y) should the next pixel be (x+1, y) or (x+1, y+1)?
  // Let minimum error decide!
  //
  // next pixel to fill = (x+1, y+1), if |f(x+1, y+1)| < |f(x+1, y)|
  //                      (x+1, y  ), otherwise
  //

  // Improvement:
  // Instead of evaluating line equation for every pixel,
  // consider the increment/decrement in the error per step:

  // f(x, y)     = (y - y1)dx - (x - x1)dy         (1)
  // f(x+1, y)   = (y - y1)dx - (x + 1 - x1)dy     (2)
  // f(x+1, y+1) = (y + 1 - y1)dx - (x + 1 - x1)dy (3)

  // Step in x:
  // (2) - (1) => (-(x + 1 - x1) - -(x - x1))dy
  //               (-x - 1 + x1 + x - x1) dy
  //               -dy

  // Step in xy:
  // (3) - (1) =>  ((y + 1 - y1) - (y - y1))dx + (-(x + 1 - x1) - -(x - x1))dy
  //               dx - dy

  // Therefore if we have current error e then:
  //   stepping in x  direction yields e_x  = e - dy
  //   stepping in xy direction yields e_xy = e + dx - dy

  const int dx = x2 - x1;
  const int dy = y2 - y1;
  IVec2s r = ivec2s_reserve(a, dx + 1);
  int err = 0;
  for (; x1 <= x2; x1++) {
    *(ivec2s_push(a, &r)) = (IVec2){ .x = x1, .y = y1, };
    int err_x  = err - dy;
    int err_xy = err + dx - dy;
    if (abs(err_xy) < abs(err_x)) {
      y1++;
      err = err_xy;
    }
    else {
      err = err_x;
    }
  }
  return r;
}

static IVec2s line_bresenham_horiz_positive_slope(Arena *a, int x1, int y1, int x2, int y2) {
  assert(x2 >= x1);
  assert(y2 >= y1);
  const int dx = x2 - x1;
  const int dy = y2 - y1;
  int err_xy = dx - dy;
  IVec2s r = ivec2s_reserve(a, dx + 1);
  for (; x1 <= x2; x1++) {
    *(ivec2s_push(a, &r)) = (IVec2){ .x = x1, .y = y1, };
    int err_x = err_xy - dx;
    if (err_x + err_xy < 0) {
      y1++;
      err_xy = err_xy + dx - dy; // err_xy for pixel (x+1, y+1)
    }
    else {
      err_xy = err_xy - dy;     // err_xy for pixel (x+1, y)
    }
  }
  return r;
}

static IVec2s line_bresenham_slow(Arena *a, int x1, int y1, int x2, int y2) {
  IVec2s r = {};

  if (x2 < x1) {
    // Swap
    int tx = x1;
    int ty = y1;
    x1 = x2;
    y1 = y2;
    x2 = tx;
    y2 = ty;
  }

  int dx = x2 - x1;
  int dy = y2 - y1;
  if (dy > 0) dy = -dy;
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    *(ivec2s_push(a, &r)) = (IVec2){ .x = x1, .y = y1, };
    if (err * 2 >= dy) {
      if (x1 == x2) break;
      err += dy;
      x1 += sx;
    }
    if (err * 2 <= dx) {
      if (y1 == y2) break;
      err += dx;
      y1 += sy;
    }
  }

  return r;
}

#endif
