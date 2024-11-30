#include "app.h"
#include "raylib.h"
#include "raymath.h"

#define min(a, b) ((a) < (b)) ? (a) : (b)
#define max(a, b) ((a) > (b)) ? (a) : (b)
#define abs(a) ((a) < 0) ? -(a) : (a)

typedef unsigned short U16;
typedef signed short   I16;
typedef unsigned int   U32;
typedef signed int     I32;

typedef struct File_Data {
  U8 *data;
  Size count;
} File_Data;

#define STATE_STRUCT_CAP (1 << 12)
typedef struct {
  Size struct_size;

  Arena *perm;
  Arena *frame;

  File_Data font_file;
  U32 codepoint_to_display;

  Camera2D camera;
  Vector2 drag_start;

} State;
State *p = 0;

typedef struct {
  U16 seg_count;
  U16 *end_count;
  U16 *start_count;
  I16 *id_delta;
  U16 *id_range_offset;
  U16 *glyph_id_array;
} Font_Format4;

typedef struct {
  I16 ascender;
  I16 descender;
  I16 advance_width_max;
  U16 number_of_h_metrics;
} Font_Horizontal_Header;

typedef struct {
  U8  *cmap_beg;
  U8  *glyf_beg;
  U32 *loca_offets; // glyph_count + 1
  U32 glyph_count;
  U16 units_per_em;
  Font_Format4 format4;
  Font_Horizontal_Header hhea;
} Font_Data;

typedef struct {
  I16 x, y;
} I16x2;

typedef struct {
  I16x2 bounds_min;
  I16x2 bounds_max;
  I16 *contour_end_idx;  // contour_count
  Vector2 *segment_points; // segment_count * 3
  I32 segment_count;
  I16 contour_count;
} Glyph_Contours;

#define READ_BE16(mem) ((((U16)(mem)[0]) << 8u)  | (((U16)(mem)[1]) << 0u))
#define READ_BE32(mem) ((((U32)(mem)[0]) << 24u) | (((U32)(mem)[1]) << 16u) | (((U32)(mem)[2]) << 8u) | (((U32)(mem)[3]) << 0u))
#define READ_BE16_MOVE(mem) ({ U16 v = READ_BE16((mem)); mem += 2; v; })
#define READ_BE32_MOVE(mem) ({ U32 v = READ_BE32((mem)); mem += 4; v; })

#include <stdio.h>
#include <string.h>
#include <errno.h>
static File_Data read_entire_file(Arena *arena, const char *filepath)
{
  FILE *f = fopen(filepath, "rb");
  if (!f) goto err;
  if (fseek(f, 0, SEEK_END) != 0) goto err;
  long len = ftell(f);
  if (len == -1l) goto err;
  if (fseek(f, 0, SEEK_SET) != 0) goto err;

  U8 *file_data = arena_alloc(arena, len + 1, 4, 1);
  fread(file_data, len, 1, f);
  file_data[len] = '\0';
  if (ferror(f)) goto err;
  fclose(f);

  File_Data r = {0};
  r.data = file_data;
  r.count = len;
  return r;

 err:
  perror(strerror(errno));
  return (File_Data){0};
}

static Font_Data font_from_memory(Arena *arena, Arena temp, U8 *file_beg,
                                  Size file_len) {

#define UNUSED __attribute__((unused))

  // Offset Table
  U16 tables_count = 0;
  {
    U8 *at = file_beg;
    UNUSED U32 version = READ_BE32_MOVE(at);
    tables_count = READ_BE16_MOVE(at);
    UNUSED U16 search_range = READ_BE16_MOVE(at);
    UNUSED U16 entry_selector = READ_BE16_MOVE(at);
    UNUSED U16 range_shift = READ_BE16_MOVE(at);
  }

  // Table Directory
  U8 *head_beg = 0;
  U8 *maxp_beg = 0;
  U8 *loca_beg = 0;
  U8 *cmap_beg = 0;
  U8 *hhea_beg = 0;
  U8 *glyf_beg = 0;
  {
    U8 *at = file_beg + 12; // table entries start
    for (U16 i = 0; i < tables_count; i++) {
      U32 table_tag      = READ_BE32_MOVE(at);
      UNUSED U32 table_checksum = READ_BE32_MOVE(at);
      U32 table_offset   = READ_BE32_MOVE(at);
      UNUSED U32 table_length = READ_BE32_MOVE(at);

      if (table_tag == READ_BE32("cmap")) {
        cmap_beg = file_beg + table_offset;
      }
      else if (table_tag == READ_BE32("head")) {
        head_beg = file_beg + table_offset;
      }
      else if (table_tag == READ_BE32("loca")) {
        loca_beg = file_beg + table_offset;
      }
      else if (table_tag == READ_BE32("glyf")) {
        glyf_beg = file_beg + table_offset;
      }
      else if (table_tag == READ_BE32("maxp")) {
        maxp_beg = file_beg + table_offset;
      }
      else if (table_tag == READ_BE32("hhea")) {
        hhea_beg = file_beg + table_offset;
      }
    }
    if (head_beg == 0) {
      TraceLog(LOG_ERROR, "Font file does not contain required 'head' table");
      return (Font_Data){0};
    }
    if (loca_beg == 0) {
      TraceLog(LOG_ERROR, "Font file does not contain Index to Location table (loca)");
      return (Font_Data){0};
    }
    if (cmap_beg == 0) {
      TraceLog(LOG_ERROR, "Font file does not contain required character to glyph index mapping table (cmap)");
      return (Font_Data){0};
    }
    if (glyf_beg == 0) {
      TraceLog(LOG_ERROR, "Font file does not contain required Glyph Data table (glyf)");
      return (Font_Data){0};
    }
    if (hhea_beg == 0) {
      TraceLog(LOG_ERROR, "Font file does not contain required horizontal header table (hhea)");
      return (Font_Data){0};
    }
  }

  struct {
    U16 loca_type;
    U16 units_per_em;
  } head = {0};
  {
    U32 magic = READ_BE32(head_beg + 12);
    U16 units_per_em = READ_BE16(head_beg + 16);
    U16 loca_type = READ_BE16(head_beg + 50);
    assert(magic == 0x5F0F3CF5);

    head.units_per_em = units_per_em;
    head.loca_type = loca_type;
  }

  struct { U16 num_glyphs; } maxp = {0};
  {
    U8 *at = maxp_beg;
    UNUSED U16 v_major = READ_BE16_MOVE(at);
    UNUSED U16 v_minor = READ_BE16_MOVE(at);
    U16 num_glyphs = READ_BE16_MOVE(at);
    UNUSED U16 max_points = READ_BE16_MOVE(at);
    UNUSED U16 max_contours = READ_BE16_MOVE(at);
    UNUSED U16 max_composite_points = READ_BE16_MOVE(at);
    UNUSED U16 max_composite_contours = READ_BE16_MOVE(at);

    maxp.num_glyphs = num_glyphs;
  }

  // Index to Location table (loca)
  U32 *loca_offsets = 0;
  {
    U8 *at = loca_beg;
    loca_offsets = new(arena, U32, maxp.num_glyphs + 1);
    if (head.loca_type == 0) {
      for (Size i = 0; i < maxp.num_glyphs + 1; i++) {
        loca_offsets[i] = READ_BE16_MOVE(at);
        loca_offsets[i] *= 2;
      }
    }
    else if (head.loca_type == 1) {
      for (Size i = 0; i < maxp.num_glyphs + 1; i++) {
        loca_offsets[i] = READ_BE32_MOVE(at);
      }
    }
    else {
      assert(0 && "corrupted"); // TODO
    }
  }

  Font_Format4 format4 = {0};
  {
    U8 *at = cmap_beg;

    UNUSED U16 cmap_version = READ_BE16_MOVE(at);
    U16 cmap_encoding_count = READ_BE16_MOVE(at);
    U32 unicode_offset = 0; {
      for (U16 i = 0; i < cmap_encoding_count; i++) {
        U16 platform_id = READ_BE16_MOVE(at);
        U16 encoding_id = READ_BE16_MOVE(at);
        U32 offset      = READ_BE32_MOVE(at);
        _Bool is_unicode = (platform_id == 0 && encoding_id == 3);
        if (is_unicode) {
          unicode_offset = offset;
          break;
        }
      }
      assert(unicode_offset > 0 && "We only support format4");
    }

    at = cmap_beg + unicode_offset;
    U16 format = READ_BE16_MOVE(at);
    UNUSED U16 length = READ_BE16_MOVE(at);
    UNUSED U16 version = READ_BE16_MOVE(at);

    assert(format == 4);
    U16 seg_count_x2 = READ_BE16_MOVE(at);
    UNUSED U16 search_range = READ_BE16_MOVE(at);
    UNUSED U16 entry_selector = READ_BE16_MOVE(at);
    UNUSED U16 range_shift = READ_BE16_MOVE(at);
    format4.seg_count = seg_count_x2 / 2;
    format4.end_count = new (arena, U16, format4.seg_count);
    for (Size i = 0; i < format4.seg_count; i++) {
      format4.end_count[i] = READ_BE16_MOVE(at);
    }
    assert(format4.end_count[format4.seg_count - 1] == 0xFFFF);
    U16 _reserved = READ_BE16_MOVE(at);
    assert(_reserved == 0);
    format4.start_count = new (arena, U16, format4.seg_count);
    for (Size i = 0; i < format4.seg_count; i++) {
      format4.start_count[i] = READ_BE16_MOVE(at);
    }
    assert(format4.start_count[format4.seg_count - 1] == 0xFFFF);
    format4.id_delta = new(arena, I16, format4.seg_count);
    for (Size i = 0; i < format4.seg_count; i++) {
      format4.id_delta[i] = (I16)READ_BE16_MOVE(at);
    }
    format4.id_range_offset = new (arena, U16, format4.seg_count);
    for (Size i = 0; i < format4.seg_count; i++) {
      format4.id_range_offset[i] = READ_BE16_MOVE(at);
    }
  }

  Font_Horizontal_Header hhea = {0};
  { // hhea - horizontal header
    U8 *at = hhea_beg;
    UNUSED U32 table_version = READ_BE32_MOVE(at);
    I16 ascender = READ_BE16_MOVE(at);
    I16 descender = READ_BE16_MOVE(at);
    UNUSED I16 line_gap = READ_BE16_MOVE(at);
    U16 advance_width_max = READ_BE16_MOVE(at);
    UNUSED I16 min_left_side_bearing = READ_BE16_MOVE(at);
    UNUSED I16 min_right_side_bearing = READ_BE16_MOVE(at);
    UNUSED I16 x_max_extent = READ_BE16_MOVE(at);
    UNUSED I16 caret_slope_rise = READ_BE16_MOVE(at);
    UNUSED I16 caret_slope_run = READ_BE16_MOVE(at);
    UNUSED U32 _reserved1 = READ_BE32_MOVE(at);
    UNUSED U32 _reserved2 = READ_BE32_MOVE(at);
    UNUSED U16 _reserved3 = READ_BE16_MOVE(at);
    UNUSED I16 metric_data_format = READ_BE16_MOVE(at);
    U16 number_of_h_metrics = READ_BE16_MOVE(at);
    assert((_reserved1 | _reserved2 | _reserved3 ) == 0);

    hhea.advance_width_max = advance_width_max;
    hhea.ascender = ascender;
    hhea.descender = descender;
    hhea.number_of_h_metrics = number_of_h_metrics;
  }

  Font_Data r = {0};
  r.units_per_em = head.units_per_em;
  r.glyph_count = maxp.num_glyphs;
  r.glyf_beg = glyf_beg;
  r.format4 = format4;
  r.hhea = hhea;
  r.loca_offets = loca_offsets;
  return r;
}

static Glyph_Contours glyph_contour(Arena *arena, Arena temp, Font_Data font,
                            U32 codepoint) {
  Font_Format4 format4 = font.format4;

  U32 glyph_index = 0;  {
    // TODO: binary search
    Size segment_idx = 0;
    for (Size i = 0; i < format4.seg_count; i++) {
      if (format4.end_count[i] > codepoint) {
        segment_idx = i;
        break;
      }
    }
    if (format4.start_count[segment_idx] <= codepoint) {
      if (format4.id_range_offset[segment_idx]) {
        assert(0 && "Not Implemented");
      }
      else {
        glyph_index = codepoint + format4.id_delta[segment_idx];
      }
    }
  }

  // Glyph data for glyph_index
  Glyph_Contours glyph = {0};
  {
    U8 *at = font.glyf_beg + font.loca_offets[glyph_index];

    I16 contour_count = (I16)READ_BE16_MOVE(at);
    I16 x_min = (I16)READ_BE16_MOVE(at);
    I16 y_min = (I16)READ_BE16_MOVE(at);
    I16 x_max = (I16)READ_BE16_MOVE(at);
    I16 y_max = (I16)READ_BE16_MOVE(at);
    glyph.bounds_min.x = x_min;
    glyph.bounds_min.y = y_min;
    glyph.bounds_max.x = x_max;
    glyph.bounds_max.y = y_max;

    _Bool is_composite = contour_count < 0;
    if (!is_composite) {
      U16 *contour_ends = new (&temp, U16, contour_count);
      for (Size i = 0; i < contour_count; i++) {
        contour_ends[i] = READ_BE16_MOVE(at);
      }

      U16 instruction_count = READ_BE16_MOVE(at);
      at += instruction_count;
      /* U8 *instructions = new(&temp, U8, instruction_count); */
      /* for (Size i = 0; i < instruction_count; i++) { */
      /*   instructions[i] = *at++; */
      /* } */

      Size points_count = contour_ends[contour_count - 1] + 1;
      U8 *flags = new (&temp, U8, points_count);
      for (Size i = 0; i < points_count; i++) {
        flags[i] = *(at++);
        U8 flag_repeat = (1 << 3);
        if (flags[i] & flag_repeat) {
          U8 repetitions = *at++;
          while (i < points_count && repetitions--) {
            i++;
            flags[i] = flags[i - 1];
          }
        }
      }

      I16 *x_coords = new(&temp, I16, points_count);
      for (Size i = 0; i < points_count; i++) {
        U8 flag_x_is_byte = (1 << 1);
        if (flags[i] & flag_x_is_byte) {
          x_coords[i] = *(at++);
          U8 flag_x_positive = (1 << 4);
          if (flags[i] & flag_x_positive) {}
          else { x_coords[i] = -x_coords[i];  }
        }
        else {
          U8 flag_x_is_same = (1 << 4);
          if (flags[i] & flag_x_is_same) {
          } else {
            x_coords[i] = (I16)READ_BE16_MOVE(at);
          }
        }
        if (i > 0) x_coords[i] += x_coords[i - 1];
      }

      I16 *y_coords = new(&temp, I16, points_count);
      for (Size i = 0; i < points_count; i++) {
        U8 flag_y_is_byte = (1 << 2);
        if (flags[i] & flag_y_is_byte) {
          y_coords[i] = *(at++);
          U8 flag_y_positive = (1 << 5);
          if (flags[i] & flag_y_positive) {}
          else { y_coords[i] = -y_coords[i];  }
        }
        else {
          U8 flag_y_is_same = (1 << 5);
          if (flags[i] & flag_y_is_same) {
          } else {
            y_coords[i] = (I16)READ_BE16_MOVE(at);
          }
        }
        if (i > 0) y_coords[i] += y_coords[i - 1];
      }

      glyph.contour_count = contour_count;
      glyph.contour_end_idx = new(&temp, I16, contour_count);
      glyph.segment_points = new(&temp, Vector2, points_count * 3);
      {
        Size j = 0;
        for (Size contour_idx = 0; contour_idx < contour_count; contour_idx++) {
          Size contour_start_idx = j;
          Size contour_end_idx = contour_ends[contour_idx];

          Vector2 prev_p_on_curve = (Vector2){ x_coords[contour_end_idx], y_coords[contour_end_idx] };

          for (; j <= contour_end_idx; j++) {
            Size prev_p_idx = j > 0 ? j - 1 : contour_end_idx;
            Size next_p_idx = j + 1 > contour_end_idx ? contour_start_idx : j + 1;
            _Bool is_on_curve = (flags[j] & (1 << 0));
            _Bool prev_on_curve = flags[prev_p_idx] & (1 << 0);
            Vector2 p = (Vector2){ x_coords[j], y_coords[j] };

#if 0
            Color col = ColorFromHSV(contour_idx * (360.f / contour_count), 1.f, 1.f);
            Color p_col = ColorBrightness(col, is_on_curve ? 0.8f : 0.2f);
            float p_radius = 4.f;
            if (j == contour_start_idx) {
              p_radius = 7.f;
            }
            else if (j == contour_end_idx) {
              p_radius = 5.f;
            }
            DrawCircleV(p, p_radius, p_col);
#endif

            if (is_on_curve) {
              if (prev_on_curve) {
                Vector2 p0 = prev_p_on_curve;
                Vector2 c0 = (Vector2){(p0.x + p.x) / 2.f, (p0.y + p.y) / 2.f};
                Vector2 p1 = p;
                glyph.segment_points[glyph.segment_count * 3 + 0] = p0;
                glyph.segment_points[glyph.segment_count * 3 + 1] = c0;
                glyph.segment_points[glyph.segment_count * 3 + 2] = p1;
                glyph.segment_count++;
                prev_p_on_curve = p1;
              } else {
                prev_p_on_curve = p;
              }
            }
            else {
              Vector2 p0 = prev_p_on_curve;
              Vector2 c0 = (Vector2){ x_coords[j], y_coords[j] };
              Vector2 p1 = (Vector2){ x_coords[next_p_idx], y_coords[next_p_idx] };

              _Bool p1_on_curve = (flags[next_p_idx] & (1 << 0));
              if (!p1_on_curve) {
                p1.x = c0.x + (p1.x - c0.x) / 2.f;
	              p1.y = c0.y + (p1.y - c0.y) / 2.f;
              }

              glyph.segment_points[glyph.segment_count * 3 + 0] = p0;
              glyph.segment_points[glyph.segment_count * 3 + 1] = c0;
              glyph.segment_points[glyph.segment_count * 3 + 2] = p1;
              glyph.segment_count++;
              prev_p_on_curve = p1;
            }
          }
          Vector2 p0 = (Vector2){ x_coords[contour_end_idx],   y_coords[contour_end_idx] };
          Vector2 p1 = (Vector2){ x_coords[contour_start_idx], y_coords[contour_start_idx] };
          if (flags[contour_end_idx] & (1 << 0)) {
            Vector2 c0 = (Vector2){(p0.x + p1.x) / 2.f, (p0.y + p1.y) / 2.f};
            glyph.segment_points[glyph.segment_count * 3 + 0] = p0;
            glyph.segment_points[glyph.segment_count * 3 + 1] = c0;
            glyph.segment_points[glyph.segment_count * 3 + 2] = p1;
            glyph.segment_count++;
          }
          glyph.contour_end_idx[contour_idx] = glyph.segment_count - 1;
        }
      }
    }
  }
  return glyph;
}

void *update(Arena *perm, Arena *frame, void *pstate)
{
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(perm, STATE_STRUCT_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = perm;
    p->frame = frame;

    p->camera.zoom = 1.f;
    {
      const char *font_filename = "./Iosevka-Medium.ttf";
      p->font_file = read_entire_file(p->perm, font_filename);
    }
    p->codepoint_to_display = 'A';
  }
  if (perm == 0 && frame == 0) { // Pre-reload
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

  BeginDrawing();
  ClearBackground(BLACK);
  BeginMode2D(p->camera);
  if (IsKeyDown(KEY_UP))   { p->camera.zoom += 0.01f; }
  if (IsKeyDown(KEY_DOWN)) { p->camera.zoom -= 0.01f; }
  if (IsKeyDown(KEY_LEFT)) {
    p->camera.target.x -= 0.07f;
    p->camera.target.y -= 0.07f;
  }
  if (IsKeyDown(KEY_RIGHT)) {
    p->camera.target.x += 0.07f;
    p->camera.target.y += 0.07f;
  }

  p->codepoint_to_display += IsKeyPressed(KEY_N);
  p->codepoint_to_display -= IsKeyPressed(KEY_P);

  Font_Data font = font_from_memory(p->frame, *p->perm, p->font_file.data,
                                    p->font_file.count);
  Glyph_Contours glyph = glyph_contour(p->frame, *p->perm, font, p->codepoint_to_display);
  { // Draw glyph

    DrawLine(0, 0, 1000, 0, WHITE);
    DrawLine(0, 0, 0, 1000, WHITE);

    Size i = 0;
    for (Size contour_idx = 0; contour_idx < glyph.contour_count; contour_idx++) {
      for (; i <= glyph.contour_end_idx[contour_idx]; i++) {
        Color col = ColorFromHSV(contour_idx * (360.0f / glyph.contour_count),
                                 1.f, 1.f);
        float dpi = 96.f;
        float point_size = 12.f;
        float pt_per_inch = 72.f; // define a point as 1/72nd of an inch.
        float conversion_factor = (dpi * point_size) / (pt_per_inch * font.units_per_em);
        Vector2 p0 = glyph.segment_points[i * 3 + 0];
        Vector2 c0 = glyph.segment_points[i * 3 + 1];
        Vector2 p1 = glyph.segment_points[i * 3 + 2];
        DrawSplineSegmentBezierQuadratic((Vector2){p0.x * conversion_factor, p0.y * conversion_factor},
                                         (Vector2){c0.x * conversion_factor, c0.y * conversion_factor},
                                         (Vector2){p1.x * conversion_factor, p1.y * conversion_factor},
                                         3.f, col);
      }
    }
  }
  EndMode2D();

  Rectangle drag_bounds = {0};
  {
    Vector2 m = GetMousePosition();
    drag_bounds.x = min(m.x, p->drag_start.x),
    drag_bounds.y = min(m.y, p->drag_start.y),
    drag_bounds.width = abs(m.x - p->drag_start.x);
    drag_bounds.height = abs(m.y - p->drag_start.y);
  }

  p->camera.offset = (Vector2) { GetRenderWidth() / 2.f, GetRenderHeight() / 2.f };
  if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
    p->drag_start = GetMousePosition();
  }
  if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
    __builtin_memset(&p->camera, 0, sizeof(p->camera));
    p->camera.zoom = 1.f;
  }
  if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
    Vector2 screen_p = (Vector2){drag_bounds.x + drag_bounds.width * 0.5f, drag_bounds.y + drag_bounds.height * 0.5f};
    Vector2 world_p = GetScreenToWorld2D(screen_p, p->camera);
    float scale = max(drag_bounds.width, drag_bounds.height);
    p->camera.target = world_p;
    if (scale > 30) {
      p->camera.zoom = (GetRenderWidth() / scale);
    }
    p->drag_start = (Vector2){0};
  }
  if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
    DrawRectangleRec(drag_bounds, BLUE);
  }
  EndDrawing();

  return p;
}
