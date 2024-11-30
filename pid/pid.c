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

#define size_of(s)  (Size)sizeof(s)
#define align_of(s) (Size)_Alignof(s)
#define count_of(s) (size_of((s)) / size_of(*(s)))

#ifndef assert
# define assert(c)  while((!(c))) __builtin_unreachable()
#endif


////////////////////////////////////////////////////////////////////////////////
//- Context

typedef void *(*Allocator_Func)(void*, void*, Size, Size);

typedef struct Context Context;
struct Context {
  Allocator_Func allocator;
  void *allocator_ctx;
  // struct Arena *scratch_pool[SCRATCH_ARENA_COUNT];
};

void *default_alloc(void *ctx, void *ptr, Size n_bytes, Size old_n_bytes);

static __thread Context ctx = {
  .allocator = default_alloc,
  .allocator_ctx = 0,
};


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
//- Program

#include <math.h>

static Arena *new_arena(Size capacity)
{
  Arena *a = malloc(sizeof(Arena) + capacity);
  a->backing = (Byte*)a + size_of(*a);
  a->capacity = capacity - size_of(*a);
  a->at = 0;
  return a;
}

#include <raylib.h>

Rectangle rectcut_top(Rectangle *rect, float amount) {
  Rectangle r = {0};
  r.x = rect->x;
  r.y = rect->y;
  r.width = rect->width;
  r.height = amount;
  rect->y += amount;
  rect->height -= amount;
  return r;
}

Rectangle rectcut_left(Rectangle *rect, float amount) {
  Rectangle r = {0};
  r.x = rect->x;
  r.y = rect->y;
  r.width = amount;
  r.height = rect->height;
  rect->x += amount;
  rect->width -= amount;
  return r;
}

typedef struct PID {
  double p_gain;

  double i_gain;
  double i_state;
  double i_min, i_max;

  double d_gain;
  double d_state;
} PID;

double update_pid(PID *pid, double error, double position)
{
  double p_term = error * pid->p_gain;
  double i_term = 0;
  double d_term = 0;
  return p_term + i_term - d_term;
}

typedef struct Motor {
  double velocity;
  double position;
} Motor;

void motor_update(Motor *m, double v_in, float dt) {
  m->velocity += (v_in - m->velocity) * 0.88;
  m->position = m->position + m->velocity * dt;
}

void motor_draw(Motor *m, double command, Rectangle boundary) {
  Rectangle inner = boundary;
  Rectangle body = rectcut_left(&inner, boundary.width * 0.8);
  DrawRectangleRec(body, GRAY);

  float radius_blade = 20;
  Vector2 rotor_end = (Vector2){body.x + body.width + inner.width - radius_blade - 3, body.y + body.height / 2};

  DrawLineV((Vector2){body.x + body.width, body.y + body.height / 2}, rotor_end,
            BLACK);

  {
    float rad = m->position + M_PI * 0.5;
    Vector2 p = {0};
    p.x = rotor_end.x + cosf(rad) * radius_blade;
    p.y = rotor_end.y + sinf(rad) * radius_blade;
    DrawLineV(rotor_end, p, BLACK);
  }

  {
    float rad = command + M_PI * 0.5;
    Vector2 p = {0};
    p.x = rotor_end.x + cosf(rad) * radius_blade;
    p.y = rotor_end.y + sinf(rad) * radius_blade;
    DrawLineV(rotor_end, p, BLUE);
  }
}

typedef struct Series {
  double *items;
  Size capacity;
  Size at;
  double min_value, max_value;
  Color col;
} Series;

static void push_sample(Series *s, double sample)
{
  if (sample < s->min_value) s->min_value = sample;
  if (sample > s->max_value) s->max_value = sample;

  s->items[s->at] = sample;
  s->at = (s->at + 1) % s->capacity;
}

static void series_plot(Series s, Rectangle boundary, double plot_range) {

  float xtick = (boundary.width / s.capacity);
  for (Size i = 0; i < s.capacity - 1; i++) {
    Size i_start = (s.at + i) % s.capacity;
    Size i_end   = (s.at + i + 1) % s.capacity;
    float x = xtick * i;
    float yval = ((s.items[i_start] ) / plot_range) * boundary.height;

    float x_end = xtick * (i + 1);
    float yval_end = ((s.items[i_end]) / plot_range) * boundary.height;
    DrawLineV(
       (Vector2){boundary.x + x, boundary.y + boundary.height - yval},
        (Vector2){boundary.x + x_end, boundary.y + boundary.height - yval_end},
              BLACK);
  }
}

int main(int argc, char **argv)
{
  Arena *heap = new_arena(1 << 18);

  Motor motor = {0};

  Series series[2] = {0};
  Series *pos_series = &series[0];
  pos_series->capacity = 1024;
  pos_series->items = new(heap, double, pos_series->capacity);

  Series *cmd_series = &series[1];
  cmd_series->capacity = 1024;
  cmd_series->items = new (heap, double, cmd_series->capacity);

  PID pid = {0};
  pid.p_gain = 10.f;
  pid.i_gain = 1.f;
  pid.d_gain = 1.f;

  double command = 0;

  InitWindow(800, 600, "PID controller");
  while (!WindowShouldClose()) {

    if (IsKeyDown(KEY_A)) {
      command = 0;
    }
    if (IsKeyDown(KEY_R)) {
      command = M_PI * 0.5f;
    }
    if (IsKeyDown(KEY_S)) {
      command = M_PI * 1.5f;
    }
    if (IsKeyDown(KEY_W)) {
      command += 0.01;
    }
    if (command > M_PI * 2) {
      command = command - M_PI * 2;
    }

    double drive = update_pid(&pid, command - motor.position, motor.position);
    motor_update(&motor, drive, GetFrameTime());

    push_sample(pos_series, motor.position);
    push_sample(cmd_series, command);

    BeginDrawing();
    ClearBackground(WHITE);

    Rectangle window = (Rectangle){0, 0, GetRenderWidth(), GetRenderHeight()};
    Rectangle cut = window;
    Rectangle model = rectcut_top(&cut, 200);
    model = rectcut_left(&model, 300);
    motor_draw(&motor, command, model);

    Rectangle plot = cut;

    double plot_min_value = 99999, plot_max_value = 0;
    for (Size i = 0; i < count_of(series); i++) {
      plot_min_value = plot_min_value < series[i].min_value ? plot_min_value : series[i].min_value;
      plot_max_value = plot_max_value > series[i].max_value ? plot_max_value : series[i].max_value;
    }
    double plot_range = plot_max_value - plot_min_value;

    {
      char temp[256];
      snprintf(temp, count_of(temp), "%.3f", plot_min_value);
      DrawText(temp, plot.x, plot.y + plot.height - 10, 1, BLACK);
      snprintf(temp, count_of(temp), "%.3f", plot_max_value);
      DrawText(temp, plot.x, plot.y, 1, BLACK);
    }

    series_plot(*pos_series, plot, plot_range);
    series_plot(*cmd_series, plot, plot_range);

    EndDrawing();
  }
  CloseWindow();

  free(heap);
  return 0;
}
