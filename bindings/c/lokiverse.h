#ifndef LOKIVERSE_H
#define LOKIVERSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lokiverse_message lokiverse_message;
typedef struct lokiverse_stream lokiverse_stream;
typedef struct lokiverse_transport lokiverse_transport;
typedef struct lokiverse_logger lokiverse_logger;

typedef struct {
  uint64_t timestamp_ns;
  char *line;
  size_t line_size;
} lokiverse_value;

typedef struct {
  int (*post)(void *context, const char *path, size_t path_size,
              const char *body, size_t body_size, const char *content_type,
              size_t content_type_size);
  void (*destroy)(void *context);
} lokiverse_transport_callbacks;

typedef struct {
  int (*push)(void *context, const lokiverse_message *message, int *success);
  void (*destroy)(void *context);
} lokiverse_logger_callbacks;

enum {
  LOKIVERSE_OK = 0,
  LOKIVERSE_INVALID_ARGUMENT = 1,
  LOKIVERSE_NO_MEMORY = 2,
  LOKIVERSE_INTERNAL = 3
};

lokiverse_message *lokiverse_message_new(void);
void lokiverse_message_free(lokiverse_message *message);
lokiverse_stream *lokiverse_message_stream(lokiverse_message *message);
int lokiverse_message_encode(const lokiverse_message *message, char **data,
                             size_t *size);
void lokiverse_buffer_free(char *data);

int lokiverse_value_now(const char *line, size_t line_size,
                        lokiverse_value *value);
void lokiverse_value_free(lokiverse_value *value);

int lokiverse_stream_reserve(lokiverse_stream *stream, size_t label_count,
                             size_t value_count);
int lokiverse_stream_label(lokiverse_stream *stream, const char *name,
                           size_t name_size, const char *value,
                           size_t value_size);
int lokiverse_stream_entry(lokiverse_stream *stream, uint64_t timestamp_ns,
                           const char *line, size_t line_size);
int lokiverse_stream_entry_now(lokiverse_stream *stream, const char *line,
                               size_t line_size);
int lokiverse_stream_commit(lokiverse_stream *stream);
void lokiverse_stream_free(lokiverse_stream *stream);

int lokiverse_transport_make_http(const char *host, size_t host_size, int port,
                                  lokiverse_transport **transport);
int lokiverse_transport_make_custom(lokiverse_transport_callbacks callbacks,
                                    void *context,
                                    lokiverse_transport **transport);
int lokiverse_transport_post(lokiverse_transport *transport, const char *path,
                             size_t path_size, const char *body,
                             size_t body_size, const char *content_type,
                             size_t content_type_size, int *status);
void lokiverse_transport_free(lokiverse_transport *transport);

int lokiverse_logger_make_safe(lokiverse_transport *transport,
                               lokiverse_logger **logger);
int lokiverse_logger_make_unsafe(lokiverse_transport *transport,
                                 lokiverse_logger **logger);
int lokiverse_logger_make_custom(lokiverse_logger_callbacks callbacks,
                                 void *context, lokiverse_logger **logger);
int lokiverse_logger_push(lokiverse_logger *logger,
                          const lokiverse_message *message, int *success);
void lokiverse_logger_free(lokiverse_logger *logger);

#ifdef __cplusplus
}
#endif

#endif
