#if IN_SHELL /* $ bash cgen.c
cc cgen.c -o cgen    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib -Wno-unused-function
cc cgen.c -o cgen.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib
# cc cgen.c -o cgen    -DAMALGAMATION -Wall -O3 -lpthread -lraylib
exit # */
#endif

// title: Immediate-mode Animation Experiment
// license: This is free and unencumbered software released into the public domain.

#include <raylib.h>
#include <raymath.h>

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
#define newend(a, t, n) ((t *) arena_alloc_end(a, size_of(t), (Size)_Alignof(t), (n)))

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

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static U8 *arena_alloc_end(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->end) & (align - 1);
  Size total   = padding + objsize * count;
  if (total >= (a->end - a->beg)) {
		TraceLog(LOG_FATAL, "Out of memory.");
  }
  U8 *p = a->end - total;
  __builtin_memset(p, 0, objsize * count);
  a->end -= total;
  return p;
}


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
  void *(*update)(Arena *, Arena *, void *);
} AppCode;

static AppCode maybe_load_or_reload_app_code(AppCode app_code, _Bool should_reload)
{
  AppCode result = app_code;
  _Bool should_init = (app_code.handle == 0);

	if (should_reload) {
   #if defined(AMALGAMATION)
   #else
			assert(app_code.handle && "Can't reload unloaded executable.");
			void *dummy = app_code.handle;
			app_code.update(0, 0, dummy); // nofify pre-reload
			dlclose(app_code.handle);
   #endif
  }
  if (should_init || should_reload) {
    result = (AppCode){0};
		#if defined(AMALGAMATION)
				result.handle = update;
				result.update = update;
		#else
		  result.handle = dlopen("./cgen.so", RTLD_NOW);
      if (result.handle) {
		    result.update = dlsym(result.handle, "update");
		    assert(result.update);
      }
      else {
        perror("dlopen");
      }
		#endif
  }

  return result;
}

int main(int argc, char **argv)
{
  Size HEAP_CAP = 1ll << 30;
  void *heap = MemAlloc(HEAP_CAP);

	Arena arena = (Arena){heap, heap + HEAP_CAP};
	Size frame_cap = 1ll << 26;
  Arena frame = {0};
  {
    frame.beg = new(&arena, U8, frame_cap);
		frame.end = frame.beg + frame_cap;
  }

	AppCode app_code = {0};
	void *app_state = 0;

	InitWindow(800,800, "Meta generator");
	SetWindowState(FLAG_WINDOW_RESIZABLE);
	while (!WindowShouldClose()) {
		app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
		app_state = app_code.update(&arena, &frame, app_state);
		frame.beg = frame.end - frame_cap; // rewind frame arena
	}
	CloseWindow();

  MemFree(heap);
  return 0;
}
#endif

////////////////////////////////////////////////////////////////////////////////
//- App Code

#if defined(AMALGAMATION) || defined(BUILD_RELOADABLE)

#define MAX_STATE_CAP (1 << 12)
#define min(a, b) ((a) < (b)) ? (a) : (b)


////////////////////////////////////////////////////////////////////////////////
//- String slice

#define S8(s)        (String8){ .buf = (U8 *)(s), .len = count_of((s)) - 1, }

typedef struct String8
{
  U8 *buf;
  Size len;
} String8;

static _Bool str_prefix_match(String8 a, String8 b) {
  Size len = a.len < b.len ? a.len : b.len;
  for (Size i = 0; i < len; i++) {
    if (a.buf[i] != b.buf[i]) { return 0; }
  }
  return !!len;
}

////////////////////////////////////////////////////////////////////////////////
//- Msgs

typedef enum
{
  MsgKind_Null,
  MsgKind_Warning,
  MsgKind_Error,
  MsgKind_FatalError,
} MsgKind;

typedef struct Msg Msg;
struct Msg
{
  Msg *next;
  struct MD_Node *node;
  MsgKind kind;
  String8 string;
};

typedef struct MsgList MsgList;
struct MsgList
{
  Msg *first;
  Msg *last;
  U64 count;
  MsgKind worst_message_kind;
};

//////////////////////
// Node
typedef enum
{
  MD_NodeKind_Nil,
  MD_NodeKind_File,
  MD_NodeKind_ErrorMarker,
  MD_NodeKind_Main,
  MD_NodeKind_Tag,
  MD_NodeKind_COUNT
} MD_NodeKind;

typedef U64 MD_NodeFlags;
enum MD_NodeFlags_Type
{
  MD_NodeFlag_MaskSetDelimiters          = (0x3F<<0),
  MD_NodeFlag_HasParenLeft               = (1<<0),
  MD_NodeFlag_HasParenRight              = (1<<1),
  MD_NodeFlag_HasBracketLeft             = (1<<2),
  MD_NodeFlag_HasBracketRight            = (1<<3),
  MD_NodeFlag_HasBraceLeft               = (1<<4),
  MD_NodeFlag_HasBraceRight              = (1<<5),

  MD_NodeFlag_MaskSeparators             = (0xF<<6),
  MD_NodeFlag_IsBeforeSemicolon          = (1<<6),
  MD_NodeFlag_IsAfterSemicolon           = (1<<7),
  MD_NodeFlag_IsBeforeComma              = (1<<8),
  MD_NodeFlag_IsAfterComma               = (1<<9),

  MD_NodeFlag_MaskStringDelimiters       = (0xF<<10),
  MD_NodeFlag_StringSingleQuote          = (1<<10),
  MD_NodeFlag_StringDoubleQuote          = (1<<11),
  MD_NodeFlag_StringTick                 = (1<<12),
  MD_NodeFlag_StringTriplet              = (1<<13),

  MD_NodeFlag_MaskLabelKind              = (0xF<<14),
  MD_NodeFlag_Numeric                    = (1<<14),
  MD_NodeFlag_Identifier                 = (1<<15),
  MD_NodeFlag_StringLiteral              = (1<<16),
  MD_NodeFlag_Symbol                     = (1<<17),
};
#define MD_NodeFlag_AfterFromBefore(f) ((f) << 1)

typedef struct MD_Node MD_Node;
struct MD_Node
{
  // rjf: tree links
  MD_Node *next;
  MD_Node *prev;
  MD_Node *parent;
  MD_Node *first;
  MD_Node *last;

  // rjf: tag links
  MD_Node *first_tag;
  MD_Node *last_tag;

  // rjf: node info
  MD_NodeKind kind;
  MD_NodeFlags flags;
  String8 string;
  String8 raw_string;

  // rjf: source code info
  U64 src_offset;
};

////////////////////////////////
// List macros
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

//- rjf: Base Doubly-Linked-List Macros
#define DLLInsert_NPZ(nil,f,l,p,n,next,prev) (CheckNil(nil,f) ? \
((f) = (l) = (n), SetNil(nil,(n)->next), SetNil(nil,(n)->prev)) :\
CheckNil(nil,p) ? \
((n)->next = (f), (f)->prev = (n), (f) = (n), SetNil(nil,(n)->prev)) :\
((p)==(l)) ? \
((l)->next = (n), (n)->prev = (l), (l) = (n), SetNil(nil, (n)->next)) :\
(((!CheckNil(nil,p) && CheckNil(nil,(p)->next)) ? (0) : ((p)->next->prev = (n))), ((n)->next = (p)->next), ((p)->next = (n)), ((n)->prev = (p))))
#define DLLPushBack_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,f,l,l,n,next,prev)
#define DLLPushFront_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,l,f,f,n,prev,next)
#define DLLRemove_NPZ(nil,f,l,n,next,prev) (((n) == (f) ? (f) = (n)->next : (0)),\
((n) == (l) ? (l) = (l)->prev : (0)),\
(CheckNil(nil,(n)->prev) ? (0) :\
((n)->prev->next = (n)->next)),\
(CheckNil(nil,(n)->next) ? (0) :\
((n)->next->prev = (n)->prev)))

#if !defined(READ_ONLY)
# if defined(_WIN32)
#  pragma section(".roglob", read)
#  define READ_ONLY __declspec(allocate(".roglob"))
# else
#  define READ_ONLY __attribute__((section(".rodata#")))
// GCC HACK:
// The # at the end of the section is a dirty hack to treat trailing assembly flags "aw" as comment.
// Otherwise the assembler gives warning: "setting incorrect section attributes for .rodata"
//
//   .section	.rodata ,"aw"
//   .section	.rodata#,"aw"
//                   ^ single line comment
//
// "aw" flags are:
//    a: section is allocatable
//    w: section is writable !!!
//
# endif
#endif

READ_ONLY MD_Node md_nil_node =
{
  &md_nil_node,
  &md_nil_node,
  &md_nil_node,
  &md_nil_node,
  &md_nil_node,
  &md_nil_node,
  &md_nil_node,
};

static void msg_list_push_ex(MsgList *msgs, Msg *msg, MD_Node *node,
                             MsgKind kind, String8 string) {
  msg->kind = kind;
  msg->string = string;
  msg->node = node;
  if (msgs->first == 0) {
    msgs->first = msgs->last = msg;
  }
  else {
    msgs->last->next = msg;
    msgs->last = msg;
  }
  msgs->count += 1;
  msgs->worst_message_kind = (kind > msgs->worst_message_kind) ? kind : msgs->worst_message_kind;
}

typedef struct MD_NodeRec MD_NodeRec;
struct MD_NodeRec
{
  MD_Node *next;
  Size push_count;
  Size pop_count;
};

static MD_Node *push_node_ex(MD_Node *node, MD_NodeKind kind, MD_NodeFlags flags,
                         String8 string, String8 raw_string, U64 src_offset)
{
  node->first = node->last = node->parent = node->next = node->prev =
    node->first_tag = node->last_tag = &md_nil_node;
  node->kind = kind;
  node->flags = flags;
  node->string = string;
  node->raw_string = raw_string;
  node->src_offset = src_offset;
  return node;
}

static void push_node_child(MD_Node *parent, MD_Node *node)
{
  node->parent = parent;
  DLLPushBack_NPZ(&md_nil_node, parent->first, parent->last, node, next, prev);
}

typedef U32 MD_TokenFlags;
enum
{
  // rjf: base kind info
  MD_TokenFlag_Identifier          = (1<<0),
  MD_TokenFlag_Numeric             = (1<<1),
  MD_TokenFlag_StringLiteral       = (1<<2),
  MD_TokenFlag_Symbol              = (1<<3),
  MD_TokenFlag_Reserved            = (1<<4),
  MD_TokenFlag_Comment             = (1<<5),
  MD_TokenFlag_Whitespace          = (1<<6),
  MD_TokenFlag_Newline             = (1<<7),

  // rjf: decoration info
  /* MD_TokenFlag_StringSingleQuote   = (1<<8), */
  /* MD_TokenFlag_StringDoubleQuote   = (1<<9), */
  /* MD_TokenFlag_StringTick          = (1<<10), */
  /* MD_TokenFlag_StringTriplet       = (1<<11), */

  // rjf: error info
  MD_TokenFlag_BrokenComment       = (1<<12),
  MD_TokenFlag_BrokenStringLiteral = (1<<13),
  MD_TokenFlag_BadCharacter        = (1<<14),
};

typedef U32 MD_TokenGroups;
enum
{
  MD_TokenGroup_Comment    = MD_TokenFlag_Comment,
  MD_TokenGroup_Whitespace = (MD_TokenFlag_Whitespace|
                              MD_TokenFlag_Newline),
  MD_TokenGroup_Irregular  = (MD_TokenGroup_Comment|
                              MD_TokenGroup_Whitespace),
  MD_TokenGroup_Regular    = ~MD_TokenGroup_Irregular,
  MD_TokenGroup_Label      = (MD_TokenFlag_Identifier|
                              MD_TokenFlag_Numeric|
                              MD_TokenFlag_StringLiteral|
                              MD_TokenFlag_Symbol),
  MD_TokenGroup_Error      = (MD_TokenFlag_BrokenComment|
                              MD_TokenFlag_BrokenStringLiteral|
                              MD_TokenFlag_BadCharacter),
};

typedef struct MD_Token MD_Token;
struct MD_Token
{
  U64 range[2]; // {start, end} offset
  MD_TokenFlags flags;
};

typedef struct MD_TokenArray MD_TokenArray;
struct MD_TokenArray
{
  MD_Token *v;
  U64 count;
};

static String8 content_string_from_token(String8 raw_string,
                                         MD_TokenFlags flags) {
  U64 n_skip = 0;
  U64 n_chop = 0;
  {
    n_skip += 1*!!(flags & MD_TokenFlag_StringLiteral);
    n_chop += 1*!!(flags & MD_TokenFlag_StringLiteral);
  }
  String8 r = raw_string;
  r = (String8){r.buf + n_skip, r.len - n_skip}; // skip
  r = (String8){r.buf, r.len - n_chop}; // chop
  return r;
}

static MD_NodeFlags node_flags_from_token_flags(MD_TokenFlags flags) {
  MD_NodeFlags r = 0;
  r |= MD_NodeFlag_Identifier*!!(flags & MD_TokenFlag_Identifier);
  r |= MD_NodeFlag_Numeric*!!(flags & MD_TokenFlag_Numeric);
  r |= MD_NodeFlag_StringLiteral*!!(flags & MD_TokenFlag_StringLiteral);
  r |= MD_NodeFlag_Symbol * !!(flags & MD_TokenFlag_Symbol);
  return r;
}

////////////////////////////////
// Text -> Tokens Types

typedef struct MD_TokenizeResult MD_TokenizeResult;
struct MD_TokenizeResult
{
  MD_TokenArray tokens;
  MsgList msgs;
};

static MD_TokenizeResult tokenize_from_text(Arena *arena, String8 text) {
  MsgList msgs = {0};

  // WARN: don't touch _arena->beg_ from here on!
  MD_TokenArray tokens = {0};
  {
    Size align = ((Size)_Alignof(MD_Token));
    Size padding = -(USize)(arena->beg) & (align - 1);
    tokens.v = (MD_Token *)(arena->beg + padding);
  }

  U8 *byte_start = text.buf;
  U8 *byte_end   = text.buf + text.len;
  U8 *byte       = byte_start;

  while (byte < byte_end) {
    MD_TokenFlags token_flags = 0;
    U8 *token_start = 0;
    U8 *token_end = 0;

    // whitespace
    _Bool is_whitespace = (*byte == ' ' || *byte == '\t' || *byte == '\v' || *byte == '\r');
    if (token_flags == 0 && is_whitespace) {
      token_flags = MD_TokenFlag_Whitespace;
      token_start = token_end = byte++;
      for (; byte <= byte_end; byte++) {
        token_end++;
        _Bool is_non_whitespace = (*byte != ' ' || *byte != '\t' || *byte != '\v' || *byte == '\r');
        if (byte <= byte_end || is_non_whitespace) { break; }
      }
    }

    // newline
    if (token_flags == 0 && *byte == '\n') {
      token_flags = MD_TokenFlag_Newline;
      token_start = token_end = byte++;
      token_end++;
    }

    // single-line comment
    if (token_flags == 0 && (byte + 1) < byte_end &&
        byte[0] == '/' &&
        byte[1] == '/') {
      token_flags = MD_TokenFlag_Comment;
      token_start = byte;
      token_end = byte + 2;
      byte += 2;
      for (; byte < byte_end; byte++) {
        token_end++;
        if (*byte == '\n')
          break;
      }
    }

    // identifier
    if (token_flags == 0 &&
        (('A' <= *byte && *byte <= 'Z') || ('a' <= *byte && *byte <= 'z') ||
         (*byte == '_'))) {
      token_flags = MD_TokenFlag_Identifier;
      token_start = byte;
      token_end = byte;
      byte += 1;
      for (; byte <= byte_end; byte++) {
        token_end++;
        _Bool is_iden = ('A' <= *byte && *byte <= 'Z') ||
                        ('a' <= *byte && *byte <= 'z') || (*byte == '_');
        if (!is_iden) {
          break;
        }
      }
    }

    // number
    if (token_flags && (('0' <= *byte && *byte <= '9'))) {
      token_flags = MD_TokenFlag_Numeric;
      token_start = byte;
      token_end = byte;
      byte += 1;
      for (; byte <= byte_end; byte++) {
        token_end++;
        if ((!('0' <= *byte && *byte <= '9'))) {
          break;
        }
      }
    }

    // string literal
    // TODO: escape
    if (token_flags && ((*byte == '\"' || *byte == '\''))) {
      U8 lit = *byte;
      token_flags = MD_TokenFlag_StringLiteral;
      // TODO: add lit type to flags
      token_start = byte;
      token_end = byte + 1;
      byte += 1;
      for (; byte <= byte_end; byte++) {
        if (byte == byte_end || *byte == '\n') {
          token_end = byte;
          token_flags |= MD_TokenFlag_BrokenStringLiteral;
          break;
        }
        if (*byte == lit) {
          token_end = byte + 1;
          byte++;
          break;
        }
      }
    }

    // reserved symbols
    if(token_flags == 0 && (*byte == '{' || *byte == '}' || *byte == '(' || *byte == ')' ||
                            *byte == '[' || *byte == ']' || *byte == '#' || *byte == ',' ||
                            *byte == '\\'|| *byte == ':' || *byte == ';' || *byte == '@')) {
      token_flags = MD_TokenFlag_Reserved;
      token_start = byte;
      token_end = byte + 1;
      byte++;
    }

    // bad character
    if (token_flags == 0) {
      token_flags = MD_TokenFlag_BadCharacter;
      token_start = byte;
      token_end = byte + 1;
      byte++;
    }

    // push valid token
    if (token_flags != 0 && token_start != 0 && token_end > token_start) {
      tokens.v[tokens.count++] = (MD_Token){
        {(U64)(token_start - byte_start), (U64)(token_end - byte_start)},
        token_flags
      };
    }

    // push errors
    if (token_flags & MD_TokenFlag_BrokenStringLiteral) {
      String8 error_string = S8("Unterminated string literal");
      MD_Node *error_marker = push_node_ex(newend(arena, MD_Node, 1), MD_NodeKind_ErrorMarker, 0, S8(""), S8(""), (token_start - byte_start));
      msg_list_push_ex(&msgs, newend(arena, Msg, 1), error_marker, MsgKind_Error, error_string);
    }
  }

  // Commit tokens and return
  arena->beg = (U8 *)(tokens.v + tokens.count);
  assert(arena->beg < arena->end);
  return (MD_TokenizeResult){tokens, msgs};
}


////////////////////////////////
// Tokens -> Tree Functions

typedef struct MD_ParseResult MD_ParseResult;
struct MD_ParseResult
{
  MD_Node *root;
  MsgList msgs;
};

static MD_ParseResult parse_tree_from_tokens(MD_TokenArray tokens, String8 filename, String8 text, Arena *arena, Arena temp) {
      //- rjf: set up outputs
      MsgList msgs = {0};
      MD_Node *root = push_node_ex(new(arena, MD_Node, 1), MD_NodeKind_File, 0,
                                   filename, text, 0);

      //- rjf: set up parse rule stack
      typedef enum MD_ParseWorkKind
      {
        MD_ParseWorkKind_Main,
        MD_ParseWorkKind_NodeOptionalFollowUp,
        /* MD_ParseWorkKind_NodeChildrenStyleScan, */
      }
      MD_ParseWorkKind;

      typedef struct MD_ParseWorkNode MD_ParseWorkNode;
      struct MD_ParseWorkNode
      {
        MD_ParseWorkNode *next;
        MD_ParseWorkKind kind;
        MD_Node *parent;
        MD_Node *first_gathered_tag;
        MD_Node *last_gathered_tag;
        MD_NodeFlags gathered_node_flags;
        I64 counted_newlines;
      };

      MD_ParseWorkNode broken_work = {0, MD_ParseWorkKind_Main, root, };
      MD_ParseWorkNode first_work = {0, MD_ParseWorkKind_Main, root, };
      MD_ParseWorkNode *work_top = &first_work;

#define MD_PushParseWork(arena, work_kind, work_parent)                        \
  do {                                                                         \
    MD_ParseWorkNode *work_node = new (arena, MD_ParseWorkNode, 1);            \
    work_node->kind = work_kind;                                               \
    work_node->parent = work_parent;                                           \
    work_node->next = work_top;                                                \
    work_top = work_node;                                                      \
  } while (0)

#define MD_PopParseWork()                                                      \
      do {                                                                     \
        work_top = work_top->next;                                             \
        if (work_top == 0) work_top = &broken_work;                            \
  } while (0)

      MD_Token *token_start = tokens.v;
      MD_Token *token_end = tokens.v + tokens.count;
      MD_Token *token = token_start;
      while (token < token_end) {
        String8 token_string = (String8){text.buf + token->range[0], token->range[1] - token->range[0]};

        // whitespace || comment -> no-op
        if ((token->flags & MD_TokenFlag_Whitespace) ||
            (token->flags & MD_TokenFlag_Comment))
        {
          token += 1;
          goto end_consume;
        }
        { // [ MD_ParseWorkKind_NodeOptionalFollowUp ]
          // ':' -> work top parent has children.
#if 0
          if (work_top->kind == MD_ParseWorkKind_NodeOptionalFollowUp &&
              str_prefix_match(token_string, S8(":")))
            {
              MD_Node *parent = work_top->parent;
              MD_PopParseWork();
              MD_PushParseWork(&temp, MD_ParseWorkKind_NodeChildrenStyleScan, parent);
              token += 1;
              goto end_consume;
            }
#endif

          // anything but ':' -> node has no children == no-op
          if (work_top->kind == MD_ParseWorkKind_NodeOptionalFollowUp) {
            MD_PopParseWork();
            goto end_consume;
          }
        }
        // [main] unexpected reserved token
        if ((work_top->kind == MD_ParseWorkKind_Main) &&
            token->flags & MD_TokenFlag_Reserved &&
            str_prefix_match(token_string, S8(":"))) {
          MD_Node *error_marker =
            push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_ErrorMarker,
                         0, token_string, token_string, token->range[0]);
          String8 error_string = S8("Unexpected reserved symbol");
          msg_list_push_ex(&msgs, new(arena, Msg, 1), error_marker, MsgKind_Error, error_string);
          token += 1;
          goto end_consume;
        }

        // [main] @tag -> create new tag
        if ((work_top->kind == MD_ParseWorkKind_Main) &&
            token->flags & MD_TokenFlag_Reserved &&
            str_prefix_match(token_string, S8("@"))) {

          if (token + 1 >= token_end ||
              !(token[1].flags & MD_TokenGroup_Label)) {
            MD_Node *error_marker =
                push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_ErrorMarker,
                             0, token_string, token_string, token->range[0]);
            String8 error_string = S8("Tag label expected after @ symbol.");
            msg_list_push_ex(&msgs, new(arena, Msg, 1), error_marker, MsgKind_Error, error_string);
            token += 1;
            goto end_consume;
          } else {
            MD_Token tag_token = token[1];
            String8 tag_name_raw =
                (String8){text.buf + tag_token.range[0],
                          tag_token.range[1] - tag_token.range[0]};
            String8 tag_name = content_string_from_token(tag_name_raw, tag_token.flags);
            MD_NodeFlags node_flags = node_flags_from_token_flags(tag_token.flags);
            MD_Node *node = push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_Tag, node_flags, tag_name,
                                         tag_name_raw, tag_token.range[0]);
            DLLPushBack_NPZ(&md_nil_node, work_top->first_gathered_tag, work_top->last_gathered_tag, node, next, prev);
            if (token + 2 < token_end &&
                token[2].flags & MD_TokenFlag_Reserved &&
                str_prefix_match((String8){text.buf + token[2].range[0], token[2].range[1] - token[2].range[0]},
                                 S8("("))) {
              token += 3;
              MD_PushParseWork(&temp, MD_ParseWorkKind_Main, node);
            } else {
              token += 2;
            }
            goto end_consume;
          }
        }

        // [main] label -> create new main
        if (work_top->kind == MD_ParseWorkKind_Main &&
            (token->flags & MD_TokenGroup_Label)) {
          String8 node_string_raw = token_string;
          String8 node_string = content_string_from_token(node_string_raw, token->flags);
          MD_NodeFlags flags = node_flags_from_token_flags(token->flags);

          work_top->gathered_node_flags = 0;
          MD_Node *node = push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_Main, flags,
                                       node_string, node_string_raw, token->range[0]);
          node->first_tag = work_top->first_gathered_tag;
          node->last_tag = work_top->last_gathered_tag;
          for (MD_Node *tag = work_top->first_gathered_tag;
               !CheckNil(&md_nil_node, tag); tag = tag->next) {
            tag->parent = node;
          }
          work_top->first_gathered_tag = work_top->last_gathered_tag =
            &md_nil_node;
          push_node_child(work_top->parent, node);
          MD_PushParseWork(&temp, MD_ParseWorkKind_NodeOptionalFollowUp, node);
          token += 1;
          goto end_consume;
        }

        // [main] {s, [s, (s -> create new main
        if (work_top->kind == MD_ParseWorkKind_Main &&
            (token->flags & MD_TokenFlag_Reserved) &&
            (str_prefix_match(token_string, S8("{")) ||
             str_prefix_match(token_string, S8("[")) ||
             str_prefix_match(token_string, S8("(")))) {

          MD_NodeFlags flags = node_flags_from_token_flags(token->flags);
          flags |=   MD_NodeFlag_HasBraceLeft*!!str_prefix_match(token_string, S8("{"));
          flags |= MD_NodeFlag_HasBracketLeft*!!str_prefix_match(token_string, S8("["));
          flags |=   MD_NodeFlag_HasParenLeft*!!str_prefix_match(token_string, S8("("));
          work_top->gathered_node_flags = 0;
          MD_Node *node = push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_Main, flags,
                                       S8(""), S8(""), token->range[0]);
          node->first_tag = work_top->first_gathered_tag;
          node->last_tag = work_top->last_gathered_tag;
          for (MD_Node *tag = work_top->first_gathered_tag;
               !CheckNil(&md_nil_node, tag); tag = tag->next) {
            tag->parent = node;
          }
          work_top->first_gathered_tag = work_top->last_gathered_tag =
            &md_nil_node;
          push_node_child(work_top->parent, node);
          MD_PushParseWork(&temp, MD_ParseWorkKind_Main, node);
          token += 1;
          goto end_consume;
        }

        // [main] }s, ]s, and )s -> pop
        if (work_top->kind == MD_ParseWorkKind_Main &&
            (token->flags & MD_TokenFlag_Reserved) &&
            (str_prefix_match(token_string, S8("}")) ||
             str_prefix_match(token_string, S8("]")) ||
             str_prefix_match(token_string, S8(")")))) {
          MD_Node *parent = work_top->parent;
          parent->flags |=   MD_NodeFlag_HasBraceRight*!!str_prefix_match(token_string, S8("}"));
          parent->flags |= MD_NodeFlag_HasBracketRight*!!str_prefix_match(token_string, S8("]"));
          parent->flags |=   MD_NodeFlag_HasParenRight*!!str_prefix_match(token_string, S8(")"));
          MD_PopParseWork();
          token += 1;
          goto end_consume;
        }

        // [node children] (s -> new main
#if 0
        if (work_top->kind == MD_ParseWorkKind_NodeChildrenStyleScan &&
            (token->flags & MD_TokenFlag_Reserved) &&
            (str_prefix_match(token_string, S8("{")) ||
             str_prefix_match(token_string, S8("[")) ||
             str_prefix_match(token_string, S8("(")))) {
          MD_Node *parent = work_top->parent;
          parent->flags |=   MD_NodeFlag_HasBraceLeft*!!str_prefix_match(token_string, S8("{"));
          parent->flags |= MD_NodeFlag_HasBracketLeft*!!str_prefix_match(token_string, S8("["));
          parent->flags |=   MD_NodeFlag_HasParenLeft*!!str_prefix_match(token_string, S8("("));
          MD_PopParseWork();
          MD_PushParseWork(&temp, MD_ParseWorkKind_Main, parent);
          token += 1;
          goto end_consume;
        }

        // [node children] newline -> pop
        if (work_top->kind == MD_ParseWorkKind_NodeChildrenStyleScan &&
            token->flags & MD_TokenFlag_Newline) {
          MD_PopParseWork();
          token += 1;
          goto end_consume;
        }
#endif

        // newline -> no-op
        if (token->flags & MD_TokenFlag_Newline) {
          token += 1;
          goto end_consume;
        }

        // no consumption -> unexpected token!
        {
          MD_Node *error_marker =
              push_node_ex(new (arena, MD_Node, 1), MD_NodeKind_ErrorMarker, 0,
                           token_string, token_string, token->range[0]);
          String8 error_string = S8("Unexpected token");
          msg_list_push_ex(&msgs, new (arena, Msg, 1), error_marker,
                           MsgKind_Error, error_string);
          token += 1;
        }

      end_consume:;
      }

      MD_ParseResult result = {
        root,
        msgs,
      };
      return result;
}


////////////////////////////////
//
#include <stdio.h>
_Bool read_entire_file(Arena *arena, char *path, U8 **data, Size *data_len)
{
  FILE *fp;
  fp = fopen(path, "r");
  if (fp) {
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);

    U8 *read_data = new(arena, U8, fsize + 1);
    fread(read_data, 1, fsize, fp);

    read_data[fsize] = 0;

    *data = read_data;
    *data_len = fsize;

    fclose(fp);
    return 1;
  }
  return 0;
}

typedef struct {
  Size struct_size;
  Arena *perm;
  Arena *frame;

  // app state
  String8 filename;
  String8 text;

} State;

State *p = 0;

U64 hash_fnv1a(void *buf, U32 len) {
  U64 hash = 0xcbf29ce484222325;
  while (--len) {
    hash ^= *(unsigned char*)buf;
    hash *= 0x00000100000001b3;
    buf++;
  }
  hash ^= hash >> 32;
  return hash;
}

static void draw_text(String8 s, int pos_x, int pos_y, int font_size, Color col,
                      Arena temp) {
  U8 *str_z = new(&temp, U8, s.len + 1);
  __builtin_memcpy(str_z, s.buf, s.len);
  str_z[s.len] = 0;
  DrawText((char *)str_z, pos_x, pos_y, font_size, col);
}

void *update(Arena *perm, Arena *frame, void *pstate) {
  _Bool is_first_frame = (pstate == 0);
  if (is_first_frame) { // Init
    p = (State *) arena_alloc(perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = perm;
    p->frame = frame;
  }
  if (perm == 0 && frame == 0) { // Pre-reload
    TraceLog(LOG_INFO, "Reload.");
    return p;
  }
  if (p == 0 || is_first_frame) {    // Post-{init,reload}
    State *prev_p = is_first_frame ? p : (State *)pstate;
    if (prev_p->struct_size != size_of(*p)) {
      TraceLog(LOG_INFO, "Resized State schema %ld -> %ld", prev_p->struct_size, size_of(*p));
    }
    p = prev_p;
    p->struct_size = size_of(*p);

    { // load file
      p->filename = S8("test.mdesk");
      char *filepath = (char *)p->filename.buf;
      if (!read_entire_file(p->perm, filepath, &p->text.buf, &p->text.len)) {
        TraceLog(LOG_ERROR, "Could not open file %s", filepath);
      }
    }
  }

  BeginDrawing();
  ClearBackground(BLACK);

  Camera2D camera = {0};
  camera.zoom = 1.f;
  BeginMode2D(camera);
  {
    SetTextLineSpacing(24);

    MD_TokenizeResult tokens_result = tokenize_from_text(p->frame, p->text);
    MD_ParseResult parse_result = parse_tree_from_tokens(tokens_result.tokens, p->filename, p->text, p->frame, *p->perm);

    U64 cursor_x = 24;
    { // Debug draw token array
      String8 text = p->text;
      for (Size token_i = 0; token_i < tokens_result.tokens.count; token_i++) {
        MD_Token token = tokens_result.tokens.v[token_i];
        float hue = (hash_fnv1a(&token, sizeof(token))) % 360;
        Color c = ColorFromHSV(hue, 1.f, 1.f);
        String8 token_str = { text.buf + token.range[0], token.range[1] - token.range[0], };
        draw_text(token_str, 10, token_i * 24, 24, c, *p->frame);
      }
    }

    cursor_x += 256;
    { // Debug draw tree
      MD_Node *root = parse_result.root;
      I64 i = 0;

      Size indent = -1;
      for (MD_Node *node = root, *next = &md_nil_node;
           !CheckNil(&md_nil_node, node);
           node = next) {
        {
          next = &md_nil_node;
          if (!CheckNil(&md_nil_node, node->first)) {
            next = node->first;
            indent += 1;
          } else {
            for (MD_Node *p = node;
                 !CheckNil(&md_nil_node, p) && p != root;
                 p = p->parent) {
              if (!CheckNil(&md_nil_node, p->next)) {
                next = p->next;
                break;
              }
            }
          }
        }

        String8 kind_string = S8("Unknown");
        switch (node->kind) {
        default: assert(0);
        case MD_NodeKind_File:        { kind_string = S8("File");        } break;
        case MD_NodeKind_ErrorMarker: { kind_string = S8("ErrorMarker"); } break;
        case MD_NodeKind_Main:        { kind_string = S8("Main");        } break;
        case MD_NodeKind_Tag:         { kind_string = S8("Tag");         } break;
        }

        Color col = ColorFromHSV(node->flags, 1.f, 1.f);
        draw_text(node->string, cursor_x + indent * 24,  i++ * 24, 24, col, *p->frame);
        draw_text(kind_string, cursor_x + (-2) * 24,  (i - 1) * 24, 24, WHITE, *p->frame);
      }
    }

    cursor_x += 256;
    { // Messages
      I64 i = 0;
      for (Msg *msg = parse_result.msgs.first;
           parse_result.msgs.worst_message_kind && msg;
           msg = msg->next) {

        char *msg_kind = "Unknown";
        switch (msg->kind) {
        default: assert(0);
        case MsgKind_Warning:     { msg_kind = "Warning";     } break;
        case MsgKind_Error:       { msg_kind = "Error";       } break;
        case MsgKind_FatalError:  { msg_kind = "FatalError";  } break;
        }

        char buffer[1024];
        Size s = snprintf(buffer, sizeof(buffer), "[%s]: %.*s %.*s\n", msg_kind,
                          (int)msg->string.len, (char *)msg->string.buf,
                          (int)msg->node->raw_string.len,
                          (char *)msg->node->raw_string.buf);
        assert(s > 0);
        draw_text((String8){(U8 *)buffer, s}, cursor_x, i++ * 24, 24, RED,
                  *p->frame);
      }
    }
  }

  EndMode2D();
  EndDrawing();

  return p;
}

#endif
