/*
 * Exploring  of Error-free API design [0] where invalid pointers ar represented by a
 * read-only "nil" sentinel over NULL. With a "nil" sentinel, "invalid" and "valid" state
 * can sometimes flow through the same computation branchlessly. This only works where the
 * computation is free from side-effects and memory writes. error-handling codepaths at
 * every junction. Traditionally, using null to represent invalid state cause hardware
 * exceptions when reading(or writing) to the pointer. Instead we use a nil sentinel to
 * ensure low-level valid reads *and* emit an out-of-band error message for handling the
 * error at a later stage. (similar to errno). In either case, writing to the pointer is a
 * hardware exception.
 *
 *
 * 1. Reading from pointers returned by the API is always valid.
 *
 * There are no null pointers so reading will never cause a hardware exception.
 * These benefits compound with linked data structures.
 *
 *   Ex: node = get_node(...);
 *       node->next->next = ...; // ok! 'next' can't be null; it's either valid or "nil".
 *
 * We must design the rest of our API such that a nil sentinel does not cause unintended
 * side effects to ensure program correctness. If successful, we can call functions
 * without explicit error handling .
 *
 *   Ex: node = get_node(...);
 *
 *       insert_node(node->next->next); // ok! The argument is probably a nil sentinel and
 *                                      //     the API should handle this gracefully.
 *
 *
 * 2. Writing to pointers returned by the API /can/ be invalid.
 *
 * A write to the nil sentinel is illegal due being storid in read-only memory. Functions
 * that mutate state must take the nil sentinel into consideration and gracefully handle
 * such cases by:
 *
 *   - returning early :: if (node_ptr == nil_node) return nil_node;
 *   - don't consume   :: for (Node *n = node_ptr; n != nil_node; n = n->next) { }
 *
 *
 * The benefit of the nil sentinel is to guarantee valid _reads_. When writing we have to
 * check for null/nil regardless to avoid hardware exception, null dereference or write to
 * read-only memory.
 *
 * Whether to use a nil sentinel vs a null pointer is a trade-off that
 * needs to be decided on a case-by-case basis.
 *
 * The downside to this approach is that it requires extra effort to initialize and
 * maintain nil sentinels over using NULL.
 *
 * [0] Allan Webster defines a "error-free" API when:
 *   1. errors are returned by value, no exceptions or hidden control flow
 *   2. error details are accessible from a side-channel.
 *   3. error values are safe to read and write --- error pointers are safe to read.
 *   4. error values don't cause side-effects when passed along.
 *
 */

#if !defined(HK_READ_ONLY)
# if defined(_WIN32)
#  pragma section(".roglob", read)
#  define HK_READ_ONLY __declspec(allocate(".roglob"))
# else
#  define HK_READ_ONLY __attribute__((section(".data.rel.ro")))
# endif
#endif

// Alternatively:
// #elif defined(__GNUC__)
// #  define HK_READ_ONLY __attribute__((section(".rodata#")))
// GCC HACK: The # at the end of the section is a dirty workaround to treat trailing
// assembly flags "aw" as comment. Otherwise the assembler gives warning: "setting
// incorrect section attributes for .rodata"
//
//   .section	.rodata ,"aw"
//   .section	.rodata#,"aw"
//                   ^ single line comment
//
// "aw" flags are:
//    a: section is allocatable
//    w: section is writable !!!
//

// Or perhaps at runtime with mmap.
// {
//   nil_node = mmap(0, sizeof(*nil_node), PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)
//   ...
// }

////////////////////////////////////////////////////////////////////////////////
//~ Error

typedef struct Error {
  struct Error *next;
  const char *source_file;
  int source_line;
  int kind;
  int severity;
  void *ctx;   // note: alternatively store application specific stuff here.
  int message_len;
  char message[] __attribute__((counted_by(message_len)));
} Error;

typedef struct ErrorList {
  Error *first, *last;
  int highest_severity; // highest severity in whole list
} ErrorList;

__thread ErrorList g_errors = {0};

#define push_error(error) push_error_((error), __FILE__, __LINE__)
static void push_error_(Error *error, const char *file, int line) {
  error->source_file = file;
  error->source_line = line;

  g_errors.highest_severity = g_errors.highest_severity > error->severity
                                  ? g_errors.highest_severity
                                  : error->severity;
  if (g_errors.first == 0) {
    g_errors.first = g_errors.last = error;
  } else {
    g_errors.last->next = error;
    g_errors.last = error;
    error->next = 0;
  }
}

#define clear_errors() __builtin_memset(&g_errors, 0, sizeof g_errors)

////////////////////////////////////////////////////////////////////////////////
//~ Example State

typedef struct Node {
  struct Node *next;
  int some_data;
} Node;

HK_READ_ONLY static Node nil_node_value = {.next = &nil_node_value};
static Node *nil_node = &nil_node_value;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Node *proc(void)
{
  Node *result = nil_node;

  static unsigned int counter = 1;
  int ok = counter++ % 2;
  if (ok) {
    Node *n = (Node *)malloc(sizeof(Node)); // for demonstration purposes
    memset(n, 0, sizeof(Node));
    n->some_data = counter;
    n->next = nil_node;
    result = n;
  }
  else {
    const char *msg = "An error message: ";
    const int msg_len = strlen(msg);
    Error *error = (Error *)malloc(sizeof(Error) + msg_len);  // for demonstration purposes
    memcpy(error->message, msg, msg_len);
    error->message[msg_len] = '0' + (counter % 10);
    error->message[msg_len + 1] = 0;
    error->message_len = msg_len + 1;
    push_error(error);
  }

  return result;
}

int main(int argc, char **argv)
{
  for (int i = 0; i < 3; i++) {
    Node *my_node = proc(); // might fail
    printf("Requesting node that might be invalid%s\n", (my_node == nil_node) ? " --- error!" : " ...");
    printf("\tmy_node->some_data = %d\n", my_node->some_data);
    printf("\tmy_node->next->next->some_data = %d\n", my_node->next->next->some_data);
  }
  printf("\nNote that dereferencing is always safe regardless of whether we had an error or not.\n");
  printf("\nTime to handle potential errors..\n");
  for (Error *e = g_errors.first; e != 0; e = e->next) {
    printf("[ERROR]: %s:%d: %s\n", e->source_file, e->source_line, e->message);
  }
  clear_errors();

  printf("\nWe are now going to write to the nil node. Expecting Segmentation fault.\n");
  Node *my_node = nil_node;
  my_node->some_data = 404; // illegal write to nil_node
  printf("If you see this message then the nil node was not placed in read-only protected memory.\n");

  return 0;
}

/* Local Variables: */
/* compile-command: "cc read_only_section.c -o read_only_section -fsanitize=undefined -g3 && ./read_only_section" */
/* End: */
