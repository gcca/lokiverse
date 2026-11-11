#include <stdint.h>
#include <stdlib.h>

#include "_cgo_export.h"
#include "lokiverse.h"

static int post(void *context, const char *path, size_t path_size,
                const char *body, size_t body_size, const char *content_type,
                size_t content_type_size) {
  return lokiverseGoPost(*(uintptr_t *)context, (char *)path, path_size,
                         (char *)body, body_size, (char *)content_type,
                         content_type_size);
}

static void destroy(void *context) {
  lokiverseGoDestroy(*(uintptr_t *)context);
  free(context);
}

int lokiverse_go_transport(uintptr_t handle, lokiverse_transport **transport) {
  uintptr_t *context = malloc(sizeof(*context));
  lokiverse_transport_callbacks callbacks = {post, destroy};
  int result;
  if (!context)
    return LOKIVERSE_NO_MEMORY;
  *context = handle;
  result = lokiverse_transport_make_custom(callbacks, context, transport);
  if (result != LOKIVERSE_OK)
    free(context);
  return result;
}
