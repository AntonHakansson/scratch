#if IN_SHELL /* $ bash spatial_hash.c
cc spatial_hash.c -o spatial_hash    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc spatial_hash.c -o spatial_hash.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib -lm -Wno-unused-function
# cc spatial_hash.c -o spatial_hash  -DAMALGAMATION -Wall -O3 -lpthread -lraylib -lm
exit # */
#endif

// title: Particle simulation using verlet integration with spatial hash accelerated broad-phase search.
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
typedef typeof((char *)0-(char *)0) Size;
typedef typeof(sizeof(0))           USize;

#define size_of(s)   (Size)sizeof(s)
#define count_of(s)  (size_of((s)) / size_of(*(s)))
#define assert(c)    while((!(c))) __builtin_trap()
#define new(a, t, n) ((t *) arena_alloc(a, size_of(t), (Size)_Alignof(t), (n)))

typedef struct { U8 *beg, *end; } Arena;

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
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
  long modtime = GetFileModTime("./spatial_hash.so");
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
		  result.handle = dlopen("./spatial_hash.so", RTLD_NOW);
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
  Size HEAP_CAP = 1ll << 30;
  void *heap = MemAlloc(HEAP_CAP);

	Arena arena = (Arena){heap, heap + HEAP_CAP};
  Arena frame = {0};
  {
	  Size frame_cap = 1ll << 26;
    frame.beg = new(&arena, U8, frame_cap);
		frame.end = frame.beg + frame_cap;
  }

	AppCode app_code = {0};
	void *app_state = 0;

  SetConfigFlags(FLAG_MSAA_4X_HINT);
	InitWindow(WIDTH, HEIGHT, "Spatial_Hash");
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

//-- Shader

#define MAX_LIGHTS  4

// Light data
typedef struct {
    int type;
    bool enabled;
    Vector3 position;
    Vector3 target;
    Color color;
    float attenuation;

    // Shader locations
    int enabledLoc;
    int typeLoc;
    int positionLoc;
    int targetLoc;
    int colorLoc;
    int attenuationLoc;
} Light;

// Light type
typedef enum {
    LIGHT_DIRECTIONAL = 0,
    LIGHT_POINT
} LightType;

static const char *vs_shader = "#version 330\n\n// Input vertex attributes\nin vec3 vertexPosition;\nin vec2 vertexTexCoord;\nin vec3 vertexNormal;\nin vec4 vertexColor;\n\n// Input uniform values\nuniform mat4 mvp;\nuniform mat4 matModel;\nuniform mat4 matNormal;\n\n// Output vertex attributes (to fragment shader)\nout vec3 fragPosition;\nout vec2 fragTexCoord;\nout vec4 fragColor;\nout vec3 fragNormal;\n\n// NOTE: Add here your custom variables\n\nvoid main()\n{\n    // Send vertex attributes to fragment shader\n    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));\n    fragTexCoord = vertexTexCoord;\n    fragColor = vertexColor;\n    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 1.0)));\n\n    // Calculate final vertex position\n    gl_Position = mvp*vec4(vertexPosition, 1.0);\n}\n";
static const char *fs_shader = "#version 330\n\n// Input vertex attributes (from vertex shader)\nin vec3 fragPosition;\nin vec2 fragTexCoord;\nin vec4 fragColor;\nin vec3 fragNormal;\n\n// Input uniform values\nuniform sampler2D texture0;\nuniform vec4 colDiffuse;\n\n// Output fragment color\nout vec4 finalColor;\n\n// NOTE: Add here your custom variables\n\n#define     MAX_LIGHTS              4\n#define     LIGHT_DIRECTIONAL       0\n#define     LIGHT_POINT             1\n\nstruct Light {\n    int enabled;\n    int type;\n    vec3 position;\n    vec3 target;\n    vec4 color;\n};\n\n// Input lighting values\nuniform Light lights[MAX_LIGHTS];\nuniform vec4 ambient;\nuniform vec3 viewPos;\n\nvoid main()\n{\n    // Texel color fetching from texture sampler\n    vec4 texelColor = texture(texture0, fragTexCoord);\n    vec3 lightDot = vec3(0.0);\n    vec3 normal = normalize(fragNormal);\n    vec3 viewD = normalize(viewPos - fragPosition);\n    vec3 specular = vec3(0.0);\n\n    vec4 tint = colDiffuse * fragColor;\n\n    // NOTE: Implement here your fragment shader code\n\n    for (int i = 0; i < MAX_LIGHTS; i++)\n    {\n        if (lights[i].enabled == 1)\n        {\n            vec3 light = vec3(0.0);\n\n            if (lights[i].type == LIGHT_DIRECTIONAL)\n            {\n                light = -normalize(lights[i].target - lights[i].position);\n            }\n\n            if (lights[i].type == LIGHT_POINT)\n            {\n                light = normalize(lights[i].position - fragPosition);\n            }\n\n            float NdotL = max(dot(normal, light), 0.0);\n            lightDot += lights[i].color.rgb*NdotL;\n\n            float specCo = 0.0;\n            if (NdotL > 0.0) specCo = pow(max(0.0, dot(viewD, reflect(-(light), normal))), 16.0); // 16 refers to shine\n            specular += specCo;\n        }\n    }\n\n    finalColor = (texelColor*((tint + vec4(specular, 1.0))*vec4(lightDot, 1.0)));\n    finalColor += texelColor*(ambient/10.0)*tint;\n\n    // Gamma correction\n    finalColor = pow(finalColor, vec4(1.0/2.2));\n}\n";


// -- App state

#define MAX_STATE_CAP (1 << 12)

typedef struct Point Point;
struct Point {
  Vector3 pos;
  Vector3 ppos;
  Vector3 acc;
};

typedef struct {
  Size struct_size;

  Arena *perm;
  Arena *frame;

  Camera3D camera;
  double time_accumulator;

  float radius; // boundary radius
  
  Point *points_array;
  Size max_points_array_count;
  Size points_array_count;

  Shader shader;
  Model point_model;
} State;

State *p = 0;

typedef struct {
  Vector3 min_pos;
  float dim;
  float spacing;
  Size cells;

  Size *grid; // size: (cells + 1)^3 + 1
  Size *particle_lookup;
} SpatialHashGrid;

typedef struct {
  Size x, y, z;
} SpatialHashGridIndex;

typedef struct SpatialHashGridIterator SpatialHashGridIterator;
struct SpatialHashGridIterator {
  Size start, end; // defines span [start, end) in SpatialHashGrid::particle_lookup table
  SpatialHashGridIterator *next;
};

static SpatialHashGrid spatial_hash_grid(Arena *frame, Vector3 min_pos, float dim, float spacing, void *points_array, Size points_count, Size stride, Size member_offset);
static SpatialHashGridIndex spatial_hash_grid_index(SpatialHashGrid *grid, float x, float y, float z);
static Size *spatial_hash_grid_get(SpatialHashGrid *grid, Size i, Size j, Size k);
static SpatialHashGridIterator *spatial_hash_grid_iterator(SpatialHashGrid *grid, Arena *arena, Vector3 pos);
  
static SpatialHashGrid spatial_hash_grid(Arena *frame, Vector3 min_pos, float dim, float spacing, void *points_array, Size points_count, Size stride, Size member_offset) {
  SpatialHashGrid grid = {0};
  grid.min_pos = min_pos;
  grid.dim = dim;
  grid.spacing = spacing;
  grid.cells = (grid.dim / grid.spacing) + 1;

  Size cells_plus_one = grid.cells + 1;
  Size grid_count = cells_plus_one * cells_plus_one * cells_plus_one + 1;
  grid.grid = new(frame, Size, grid_count);

  // Place points in grid
  for (Size i = 0; i < points_count; i++) {
    Vector3 *p = (Vector3 *)(points_array + i * stride + member_offset);
    SpatialHashGridIndex idx = spatial_hash_grid_index(&grid, p->x, p->y, p->z);
    (*spatial_hash_grid_get(&grid, idx.x, idx.y, idx.z))++;
  }

  // Prefix sum
  Size sum = 0;
  for (Size i = 0; i < grid_count; i++) {
    sum += grid.grid[i];
    grid.grid[i] = sum;
  }

  // Build lookup table
  grid.particle_lookup = new(frame, Size, points_count);
  for (Size i = 0; i < points_count; i++) {
    Vector3 *p = (Vector3 *)(points_array + i * stride + member_offset);
    SpatialHashGridIndex idx = spatial_hash_grid_index(&grid, p->x, p->y, p->z);
    Size lookup_i = --(*spatial_hash_grid_get(&grid, idx.x, idx.y, idx.z));
    grid.particle_lookup[lookup_i] = i;
  }
    
  return grid;
}

static SpatialHashGridIndex spatial_hash_grid_index(SpatialHashGrid *grid, float x, float y, float z) {
  SpatialHashGridIndex r = {0};
  r.x = (Size)((x - grid->min_pos.x) / grid->spacing);
  r.y = (Size)((y - grid->min_pos.y) / grid->spacing);
  r.z = (Size)((z - grid->min_pos.z) / grid->spacing);
  return r;
}
  
static Size *spatial_hash_grid_get(SpatialHashGrid *grid, Size i, Size j, Size k) {
  return &grid->grid[ ((i + 1) * (grid->cells * grid->cells)) + ((j + 1) * grid->cells) + (k + 1) ];
}

static SpatialHashGridIterator *spatial_hash_grid_iterator(SpatialHashGrid *grid, Arena *arena, Vector3 pos) {
  SpatialHashGridIterator *prev = 0;
  for (Size dxi = -1; dxi <= 1; dxi++) {
    for (Size dyi = -1; dyi <= 1; dyi++) {
      for (Size dzi = -1; dzi <= 1; dzi++) {
        SpatialHashGridIndex index = spatial_hash_grid_index(grid, pos.x, pos.y, pos.z);
        index.x += dxi;
        index.y += dyi;
        index.z += dzi;
        Size start = *spatial_hash_grid_get(grid, index.x, index.y, index.z + 0);
        Size end   = *spatial_hash_grid_get(grid, index.x, index.y, index.z + 1);

        SpatialHashGridIterator *node = new(arena, SpatialHashGridIterator, 1);
        node->start = start;
        node->end = end;
        node->next = prev;
        prev = node;
      }
    }
  }
  return prev;
}

void *update(App_Update_Params params, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(params.perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = params.perm;
    p->frame = params.frame;

    p->max_points_array_count = (1 << 10);
    p->points_array = new(p->perm, Point, p->max_points_array_count);

    p->shader = LoadShaderFromMemory(vs_shader, fs_shader);
    p->point_model = LoadModelFromMesh(GenMeshSphere(1.f, 8, 8));
    p->point_model.materials[0].shader = p->shader;
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

  // Settings
  {
    p->camera.position = (Vector3){ 0.0f, 32.0f, 90.0f };
    p->camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    p->camera.fovy = 40;
    p->radius = 32.f;
  }

  // User Input
  {
    if (IsKeyPressed(KEY_BACKSPACE)) {
      p->points_array_count = 0;
    }
  
    if ((p->points_array_count < p->max_points_array_count) &&
        (IsKeyPressed(KEY_W) || IsKeyDown(KEY_Q))) {
      static double last_spawn_timestamp = 0;
      if (GetTime() - last_spawn_timestamp > 0.04) {
        last_spawn_timestamp = GetTime();
        Point *point = &p->points_array[p->points_array_count++];
        float step =  .3f;
        float theta = p->points_array_count * step;
        Vector3 dir = Vector3Normalize((Vector3){ cosf(theta), -1.f, sinf(theta), });
        float t = cosf(0.8f * last_spawn_timestamp) * .5f + .5f;
        point->pos = (Vector3){dir.x * (t * 24.f + 6.f), p->radius * .8f, dir.z * (t * 24.f + 6.f)};
        point->ppos = Vector3Subtract(point->pos, Vector3Scale(dir, 0.8f));
      }
    }
  }

  SpatialHashGrid grid =
    spatial_hash_grid(p->frame,
                      Vector3Scale(Vector3One(), -p->radius * 1.5),
                      p->radius * 3,
                      2.0f,
                      p->points_array,
                      p->points_array_count,
                      size_of(Point),
                      offsetof(Point, pos));

  float dt = 1.f / 144.f;
  p->time_accumulator += GetFrameTime();
  while (p->time_accumulator >= dt) {
    p->time_accumulator -= dt;

    for (Size i = 0; i < p->points_array_count; i++) {
      Point *point = &p->points_array[i];
      point->acc = (Vector3){0, -100.f, 0};

      // Attract to center according to user intention
      {
        if (IsKeyDown(KEY_SPACE)) {
          Vector3 disp = Vector3Subtract(Vector3Zero(), point->pos);
          Vector3 dir = Vector3Normalize(disp);
          float dist = Vector3Length(disp);
          point->acc = Vector3Add(point->acc, Vector3Scale(dir, .6f * (dist * dist)));
        }
      }

      // Neighbor collision
      {
        for (SpatialHashGridIterator *it = spatial_hash_grid_iterator(&grid, p->frame, point->pos); it; it = it->next) {
          for (Size other_i = it->start; other_i < it->end; other_i++) {
            Size other_idx = grid.particle_lookup[other_i];
            if (other_idx == i) continue;
            Point *other = &p->points_array[other_idx];
            Vector3 disp = Vector3Subtract(other->pos, point->pos);
            float dist = Vector3Length(disp);
            if (dist < 2.f) {
              Vector3 dir = Vector3Normalize(disp);
              float intersection_amount = 2.f - dist;
              float delta = intersection_amount * .5f;
              other->pos = Vector3Add(other->pos,      Vector3Scale(dir, delta));
              point->pos = Vector3Subtract(point->pos, Vector3Scale(dir, delta));
            }
          }
        }
      }
      
      // Boundary collision
      {
        Vector3 disp = Vector3Subtract(Vector3Zero(), point->pos);
        float dist = Vector3Length(disp);
        if (dist + 1.0f > p->radius) {
          Vector3 dir = Vector3Normalize(disp);
          point->pos = Vector3Scale(dir, -(p->radius - 1.f));
        }
      }
    }

    // Verlet integration
    {
      for (Size i = 0; i < p->points_array_count; i++) {
        Point *point = &p->points_array[i];
        Vector3 vel = Vector3Subtract(point->pos, point->ppos);
        Vector3 at = Vector3Scale(point->acc, dt * dt);
        point->ppos = point->pos;
        point->pos = Vector3Add(point->pos, Vector3Add(vel, at));
        point->acc = Vector3Zero();
      }
    }
  }

  // Update Shader values
  {
    float camera_pos[3] = { p->camera.position.x, p->camera.position.y, p->camera.position.z, };
    p->shader.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(p->shader, "viewPos");
    SetShaderValue(p->shader, p->shader.locs[SHADER_LOC_VECTOR_VIEW], camera_pos, SHADER_UNIFORM_VEC3);

    int ambient_loc = GetShaderLocation(p->shader, "ambient");
    SetShaderValue(p->shader, ambient_loc, (float[4]){ 0.1, 0.1, 0.1, 1.0f }, SHADER_UNIFORM_VEC4);

    Light light = { 0 }; {
      int light_index = 0;
      light.enabled = true;
      light.type = LIGHT_DIRECTIONAL;
      light.position = Vector3Scale(Vector3One(), p->radius * 1.5f);
      light.target = Vector3Zero();
      light.color = WHITE;
      light.enabledLoc = GetShaderLocation(p->shader, TextFormat("lights[%i].enabled", light_index));
      light.typeLoc = GetShaderLocation(p->shader, TextFormat("lights[%i].type", light_index));
      light.positionLoc = GetShaderLocation(p->shader, TextFormat("lights[%i].position", light_index));
      light.targetLoc = GetShaderLocation(p->shader, TextFormat("lights[%i].target", light_index));
      light.colorLoc = GetShaderLocation(p->shader, TextFormat("lights[%i].color", light_index));

      float position[3] = { light.position.x, light.position.y, light.position.z };
      float target[3] = { light.target.x, light.target.y, light.target.z };
      float color[4] = { (float)light.color.r/(float)255, (float)light.color.g/(float)255,
                         (float)light.color.b/(float)255, (float)light.color.a/(float)255 };
      SetShaderValue(p->shader, light.enabledLoc, &light.enabled, SHADER_UNIFORM_INT);
      SetShaderValue(p->shader, light.typeLoc, &light.type, SHADER_UNIFORM_INT);
      SetShaderValue(p->shader, light.positionLoc, position, SHADER_UNIFORM_VEC3);
      SetShaderValue(p->shader, light.targetLoc, target, SHADER_UNIFORM_VEC3);
      SetShaderValue(p->shader, light.colorLoc, color, SHADER_UNIFORM_VEC4);
    }
  }

  BeginDrawing();
  ClearBackground(BLACK);

  BeginMode3D(p->camera);

  // Draw points
  for (Size i = 0; i < p->points_array_count; i++) {
    Point *point = &p->points_array[i];
    Color c = {0}; {
      Vector3 vel = Vector3Subtract(point->pos, point->ppos);
      float t = Vector3Length(vel) / (p->radius * 0.02);
      float hue = 360 - t * 360.f;
      if (hue < 1.0f) hue = 0.f;
      float saturation = t * 0.1 + 0.8;
      if (saturation > 1.0f) saturation = 1.f;
      c = ColorFromHSV(hue, saturation, 1.f);
    }
    DrawModel(p->point_model, point->pos, 1.f, c);
  }
  
  EndMode3D();
  EndDrawing();

  {
    static FFMPEG *ffmpeg = 0;
    static U8 is_recording = 0;
    if (IsKeyPressed(KEY_P)) {
      is_recording = !is_recording;
      TraceLog(LOG_INFO, "Recording %s", is_recording ? "on" : "off");
      if (is_recording) {
        ffmpeg = ffmpeg_start_rendering(WIDTH, HEIGHT, 60);
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

#endif
