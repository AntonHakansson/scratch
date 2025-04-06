#if IN_SHELL /* $ bash halfedge.c
cc halfedge.c -o halfedge    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc halfedge.c -o halfedge.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib -lm -Wno-unused-function
# cc halfedge.c -o halfedge  -DAMALGAMATION -Wall -O3 -lpthread -lraylib -lm
exit # */
#endif

// title: Fiddling with Halfedge data structure
// license: This is free and unencumbered software released into the public domain.

#include <stddef.h>
#include <raylib.h>
#include <raymath.h>

#define WIDTH  800
#define HEIGHT 800

typedef unsigned char U8;
typedef unsigned long U32;
typedef unsigned long long U64;
typedef          long long I64;
typedef typeof((char *)0-(char *)0) Iz;
typedef typeof(sizeof(0))           Uz;

#define size_of(s)   (Iz)sizeof(s)
#define count_of(s)  (size_of((s)) / size_of(*(s)))
#define assert(c)    while((!(c))) __builtin_trap()
#define new(a, t, n) ((t *) arena_alloc(a, size_of(t), (Iz)_Alignof(t), (n)))

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
	void *update(App_Update_Params, void *);
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
  long modtime = GetFileModTime("./halfedge.so");
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
		  result.handle = dlopen("./halfedge.so", RTLD_NOW);
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
	InitWindow(WIDTH, HEIGHT, "Halfedge");
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

#define da_init(a, t, cap) ({                                           \
      t *s = new(a, t, 1);                                              \
      s->capacity = cap;                                                \
      s->items = (typeof(*s->items) *)                                  \
        arena_alloc((a), sizeof(*(s)->items), _Alignof(*(s)->items), cap); \
      s;                                                                \
  })

#define da_push(a, s) ({                                                \
      typeof(s) _s = s;                                                 \
      if (_s->count >= _s->capacity) {                                    \
        da_grow((a), (void **)&_s->items, &_s->capacity, &_s->count,      \
                sizeof(*_s->items), _Alignof(*_s->items));              \
      }                                                                 \
      _s->items + _s->count++;                                            \
    })

void da_grow(Arena *arena, void **__restrict items, Iz *__restrict capacity, Iz *__restrict count, Iz item_size, Iz align)
{
  U8 *items_end = (((U8*)(*items)) + (item_size * (*count)));
  if (arena->beg == items_end) {
    // Extend in place, no allocation occured between da_grow calls
    arena_alloc(arena, item_size, align, (*capacity));
    *capacity *= 2;
  }
  else {
    // Relocate array
    *capacity += !(*capacity);
    U8 *p = arena_alloc(arena, item_size, align, (*capacity) * 2);
    if (*items) __builtin_memcpy(p, *items, (*count) * item_size);
    *items = (void *)p;
    *capacity *= 2;
  }
}

#define free_list_pop(freelist) ({ \
  __typeof(freelist) s = freelist; \
  freelist = freelist->next;  \
  s; \
})

////////////////////////////////////////////////////////////////////////////////
//- Halfedge data structure

typedef struct HE_Edge HE_Edge;
typedef struct HE_Face HE_Face;
typedef struct HE_Vertex HE_Vertex;

struct HE_Vertex {
  Vector2 pos;
  HE_Edge *edge; // One of the half-edges starting at this vertex
  HE_Vertex *next;
};

struct HE_Edge {
  HE_Vertex *end;  // Vertex at the end of the half-edge
  HE_Edge   *twin; // Oppositely oriented adjacent half-edge
  HE_Face *face;
  HE_Edge *next;
};

struct HE_Face {
  HE_Edge *edge; // One of the half-edges bordering the face
  HE_Face *next;
};

typedef struct HE_Vertices HE_Vertices;
struct HE_Vertices {
  HE_Vertex **items;
  Iz count;
  Iz capacity;
};

typedef struct Vector2s Vector2s;
struct Vector2s {
  Vector2 *items;
  Iz count;
  Iz capacity;
};

// -- App state

#define MAX_STATE_CAP (1 << 12)
typedef struct Faces {
  HE_Vertex *vertices_array;
  HE_Vertex *vertices_first_free;
  Iz max_vertices_count;

  HE_Edge *edges_array;
  HE_Edge *edges_first_free;
  Iz max_edges_count;

  HE_Face *faces_array;
  HE_Face *faces_first_free;
  Iz max_faces_count;

  HE_Vertices selected_vertices;
  Vector2s selected_vertices_orig_pos;

  HE_Face *first;
} Faces;

void he_init(Faces *f, Arena *arena) {
  f->max_vertices_count = 1024;
  f->vertices_array = f->vertices_first_free = new(arena, HE_Vertex, f->max_vertices_count);
  for (Iz i = 1; i < f->max_vertices_count; i++) { f->vertices_array[i - 1].next = &f->vertices_array[i]; }

  f->max_edges_count = f->max_vertices_count * 2;
  f->edges_array = f->edges_first_free = new(arena, HE_Edge, f->max_edges_count);
  for (Iz i = 1; i < f->max_edges_count; i++) { f->edges_array[i - 1].next = &f->edges_array[i]; }

  f->max_faces_count = f->max_vertices_count / 2;
  f->faces_array = f->faces_first_free = new(arena, HE_Face, f->max_faces_count);
  for (Iz i = 1; i < f->max_faces_count; i++) { f->faces_array[i - 1].next = &f->faces_array[i]; }
}

HE_Edge *he_connect(Faces *f, HE_Face *face, HE_Vertex *a, HE_Vertex *b) {
  HE_Edge *result = free_list_pop(f->edges_first_free);
  HE_Edge *twin   = free_list_pop(f->edges_first_free);

  a->edge = result;

  result->face = face;
  result->end = b;
  result->twin = twin;

  twin->end = a;
  twin->twin = result;

  return result;
}

HE_Face *he_closed_ccw(Faces *f, Vector2 *points, Iz count, Arena temp) {
  HE_Face *result = free_list_pop(f->faces_first_free);
  HE_Vertex **vs = new(&temp, HE_Vertex *, count + 1);
  for (Iz i = 0; i < count; i++) {
    vs[i] = free_list_pop(f->vertices_first_free);
    vs[i]->pos = points[i];
  }
  vs[count] = vs[0];
  for (Iz i = 0; i < count; i++) {
    he_connect(f, result, vs[i + 0], vs[i + 1]);
  }
  for (Iz i = 0; i < count; i++) {
    vs[i + 0]->edge->next = vs[i + 1]->edge;
    vs[i + 1]->edge->twin->next = vs[i + 0]->edge->twin;
  }
  result->edge = vs[0]->edge;
  return result;
}

typedef struct {
  Iz struct_size;

  Arena *perm;
  Arena *frame;

  double time_accumulator;

  Camera2D camera;
  Faces faces;
} State;

static State *p = 0;

static float exp_decay(float a, float b, float decay, float dt)
{
  return b + (a - b) * exp(-decay * dt);
}

static Vector2 exp_decay_v(Vector2 a, Vector2 b, float decay, float dt)
{
  Vector2 r = {};
  r.x = exp_decay(a.x, b.x, decay, dt);
  r.y = exp_decay(a.y, b.y, decay, dt);
  return r;
}

static float smooth_step(float x, float edge0, float edge1) {
  float t = (x - edge0) / (edge1 - edge0);
  t = __builtin_fmax(t, 0.f);
  t = __builtin_fmin(t, 1.f);
  return t * t * (3.0f - 2.0f * t);
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

_Bool check_point_segment_intersection(Vector2 a, Vector2 b, Vector2 p,
                              float inside_radius, Vector2 *intersection) {
  Vector2 dummy;
  if (!intersection) { intersection = &dummy; }

  Vector2 disp = Vector2Subtract(b, a);
  Vector2 np = Vector2Subtract(p, a);

  float t = Vector2DotProduct(disp, np) / Vector2LengthSqr(disp);
  t = t < 0.f ? 0.0f : t;
  t = t > 1.f ? 1.0f : t;

  Vector2 closest_point = Vector2Add(a, Vector2Scale(disp, t));
  *intersection = closest_point;
  float distance = Vector2Distance(p, closest_point);

  return distance <= inside_radius;
}

void draw_arrow(Vector2 beg, Vector2 end, Color color, float offset) {
  float gap = 40.f;

  Vector2 disp = Vector2Subtract(end, beg);
  Vector2 dir = Vector2Normalize(disp);
  Vector2 tangent = { dir.y, -dir.x };

  beg = Vector2Add(beg, Vector2Scale(dir, gap));
  beg = Vector2Add(beg, Vector2Scale(tangent, offset));
  end = Vector2Add(end, Vector2Scale(dir, -gap));
  end = Vector2Add(end, Vector2Scale(tangent, offset));

  DrawLineV(beg, end, color);

  // Draw arrow head
  {
    float arrow_height = 24.f;
    float arrow_width = 12.f;
    Vector2 v1 = Vector2Add(end, Vector2Scale(dir, -arrow_height));
    Vector2 v2 = end;
    Vector2 v3 = Vector2Add(v1, Vector2Scale(tangent, arrow_width));
    DrawTriangle(v1, v2, v3, color);
  }
}

void *update(App_Update_Params params, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(params.perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = params.perm;
    p->frame = params.frame;

    p->camera.offset = (Vector2){ GetRenderWidth() * .5f, GetRenderHeight() * .5f };
    p->camera.zoom = 1.0f;

    {
      Faces *f = &p->faces;
      he_init(f, p->perm);

      {
        float radius = 50.f;
        Vector2 ps[4] = {
          (Vector2){ +radius, +radius },
          (Vector2){ +radius, -radius },
          (Vector2){ -radius, -radius },
          (Vector2){ -radius, +radius },
        };
        f->first = he_closed_ccw(f, ps, count_of(ps), *p->frame);
      }

      {
        float radius = 250.f;
        Vector2 ps[4] = {
          (Vector2){ +radius, +radius },
          (Vector2){ +radius, -radius },
          (Vector2){ -radius, -radius },
          (Vector2){ -radius, +radius },
        };
        f->first->next = he_closed_ccw(f, ps, count_of(ps), *p->frame);
      }
    }
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

  float dt = GetFrameTime();
  
  _Bool mouse_consumed = 0;
  _Bool edge_made = 0;
  p->camera = camera_update(p->camera, &mouse_consumed);

  Vector2 mouse_pos = GetScreenToWorld2D(GetMousePosition(), p->camera);

  static Rectangle selected_region = {0};
  #define has_selected_region (selected_region.width != 0 && selected_region.height != 0)
  if (!mouse_consumed) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      selected_region.x = mouse_pos.x;
      selected_region.y = mouse_pos.y;
      p->faces.selected_vertices.count = p->faces.selected_vertices_orig_pos.count = 0;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
      for (HE_Face *f = p->faces.first; has_selected_region && f; f = f->next) {
        HE_Edge *e = f->edge;
        if (e == 0) continue;
        do {
          HE_Vertex *v = e->end;
          if (CheckCollisionPointRec(v->pos, selected_region)) {
            _Bool in_selected_vertices = 0;
            for (Iz i = 0; i < p->faces.selected_vertices.count; i++) {
              if (v == p->faces.selected_vertices.items[i]) {
                in_selected_vertices = 1;
                break;
              }
            }
            if (!in_selected_vertices) {
              *da_push(p->perm, &p->faces.selected_vertices) = v;
              *da_push(p->perm, &p->faces.selected_vertices_orig_pos) = v->pos;
            }
          }
        } while((e = e->next) && e != f->edge);
      }
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
      selected_region.width = mouse_pos.x - selected_region.x;
      selected_region.height = mouse_pos.y - selected_region.y;
      mouse_consumed = has_selected_region;
    }
    else {
      selected_region = (Rectangle){0};
    }
  }

  static Rectangle dragged_region = {0};
  #define is_dragging (dragged_region.width != 0 && dragged_region.height != 0)
  if (!mouse_consumed) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      dragged_region.x = mouse_pos.x;
      dragged_region.y = mouse_pos.y;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      for (Iz i = 0; i < p->faces.selected_vertices.count; i++) {
        p->faces.selected_vertices_orig_pos.items[i] = p->faces.selected_vertices.items[i]->pos;
      }
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      dragged_region.width = mouse_pos.x - dragged_region.x;
      dragged_region.height = mouse_pos.y - dragged_region.y;
    }
    else {
      dragged_region = (Rectangle){0};
    }
    mouse_consumed = !!p->faces.selected_vertices.count;
  }

  if (is_dragging) {
    for (Iz i = 0; i < p->faces.selected_vertices.count; i++) {
      HE_Vertex *v = p->faces.selected_vertices.items[i];
      v->pos = Vector2Add(p->faces.selected_vertices_orig_pos.items[i], (Vector2){dragged_region.width, dragged_region.height});
    }
  }
  
  BeginDrawing();
  ClearBackground(BLACK);
  BeginMode2D(p->camera);
  {
    if (has_selected_region) {
      DrawRectangleRec(selected_region, BLUE);
      DrawRectangleLinesEx(selected_region, 6, DARKBLUE);
    }
    for (HE_Face *f = p->faces.first; f; f = f->next) {
      HE_Edge *e = f->edge;
      if (e == 0) continue;
      do {
        Color inner_color = GRAY;
        Color outer_color = BLACK;

        for (Iz i = 0; i < p->faces.selected_vertices.count; i++) {
          if (e->end == p->faces.selected_vertices.items[i]) {
            outer_color = DARKBLUE;
          }
        }

        // Add new Vertex
        if (!mouse_consumed) {
          Vector2 intersection;
          _Bool collision = check_point_segment_intersection(e->end->pos, e->twin->end->pos, mouse_pos, 50.f, &intersection);
          if (!edge_made && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && collision) {
            edge_made = 1;

            HE_Vertex *v = free_list_pop(p->faces.vertices_first_free);
            v->pos = intersection;

            HE_Edge *edge = free_list_pop(p->faces.edges_first_free);
            edge->end = e->end;
            edge->face = e->face;
            edge->next = e->next;

            HE_Edge *twin = free_list_pop(p->faces.edges_first_free);
            twin->end = v;
            twin->face = e->twin->face;
            twin->next = e->twin;

            v->edge = edge;
            edge->twin = twin;
            twin->twin = edge;

            if (e->next && e->next->twin) { e->next->twin->next = twin; }
            e->end = v;
            e->next = edge;
          }
        }

        draw_arrow(e->twin->end->pos, e->end->pos, e->face       ? BLUE : RED, +6.f);
        draw_arrow(e->end->pos, e->twin->end->pos, e->twin->face ? BLUE : RED, +6.f);

        DrawCircleV(e->end->pos, 10.f, outer_color);
        DrawCircleV(e->end->pos, 6.f, inner_color);

        e = e->next;
      } while(e && e != f->edge);
    }
  }
  EndMode2D();
  EndDrawing();

  return p;
}

#endif
