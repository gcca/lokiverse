#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lokiverse.h"

static int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                     \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static int closed_port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  int port = 0;
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return 0;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0 &&
      getsockname(descriptor, (struct sockaddr *)&address, &size) == 0) {
    port = ntohs(address.sin_port);
  }
  close(descriptor);
  return port;
}

static int make_http_logger(const char *host, size_t size, int port,
                            lokiverse_logger **logger) {
  lokiverse_transport *transport = NULL;
  int status;
  if (!logger)
    return LOKIVERSE_INVALID_ARGUMENT;
  *logger = NULL;
  status = lokiverse_transport_make_http(host, size, port, &transport);
  if (status == LOKIVERSE_OK) {
    status = lokiverse_logger_make_safe(transport, logger);
  }
  lokiverse_transport_free(transport);
  return status;
}

static lokiverse_message *sample(void) {
  lokiverse_message *message = lokiverse_message_new();
  lokiverse_stream *stream = lokiverse_message_stream(message);
  CHECK(message != NULL);
  CHECK(stream != NULL);
  CHECK(lokiverse_stream_reserve(stream, 1, 2) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_label(stream, "job", 3, "c", 1) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_entry(stream, UINT64_MAX, "a\0b", 3) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_entry(stream, 0, NULL, 0) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_OK);
  lokiverse_stream_free(stream);
  return message;
}

static void test_encode(void) {
  lokiverse_message *first = sample();
  lokiverse_message *second = sample();
  char *one = (char *)1;
  char *two = NULL;
  size_t one_size = 99;
  size_t two_size = 0;
  CHECK(lokiverse_message_encode(first, &one, &one_size) == LOKIVERSE_OK);
  CHECK(lokiverse_message_encode(second, &two, &two_size) == LOKIVERSE_OK);
  CHECK(one != NULL && one_size > 0);
  CHECK(one_size == two_size);
  CHECK(one && two && memcmp(one, two, one_size) == 0);
  lokiverse_buffer_free(one);
  lokiverse_buffer_free(two);
  lokiverse_message_free(first);
  lokiverse_message_free(second);
}

static void test_empty_message(void) {
  lokiverse_message *message = lokiverse_message_new();
  char *data = (char *)1;
  size_t size = 99;
  CHECK(lokiverse_message_encode(message, &data, &size) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(data == NULL);
  CHECK(size == 0);
  lokiverse_message_free(message);
}

static void test_commit_requires_entries(void) {
  lokiverse_message *message = lokiverse_message_new();
  lokiverse_stream *stream = lokiverse_message_stream(message);
  char *data = NULL;
  size_t size = 0;
  CHECK(lokiverse_stream_label(stream, "job", 3, "c", 1) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_message_encode(message, &data, &size) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_entry_now(stream, "now", 3) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_message_encode(message, &data, &size) == LOKIVERSE_OK);
  CHECK(size > 0);
  lokiverse_buffer_free(data);
  lokiverse_stream_free(stream);
  lokiverse_message_free(message);
}

static void test_null_arguments(void) {
  lokiverse_message *message = sample();
  lokiverse_stream *stream = lokiverse_message_stream(message);
  lokiverse_logger *logger = (lokiverse_logger *)1;
  char *data = NULL;
  size_t size = 0;
  int success = 1;
  CHECK(lokiverse_message_stream(NULL) == NULL);
  CHECK(lokiverse_message_encode(NULL, &data, &size) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_message_encode(message, NULL, &size) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_message_encode(message, &data, NULL) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_label(NULL, "a", 1, "b", 1) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_label(stream, NULL, 1, "b", 1) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_label(stream, "a", 1, NULL, 1) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_entry(NULL, 1, "a", 1) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_entry(stream, 1, NULL, 1) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_entry_now(NULL, "a", 1) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_entry_now(stream, NULL, 1) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_commit(NULL) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(make_http_logger(NULL, 9, 3100, &logger) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(logger == NULL);
  CHECK(make_http_logger("localhost", 9, 3100, NULL) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_logger_push(NULL, message, &success) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(success == 0);
  lokiverse_stream_free(stream);
  lokiverse_stream_free(NULL);
  lokiverse_message_free(NULL);
  lokiverse_logger_free(NULL);
  lokiverse_buffer_free(NULL);
  lokiverse_message_free(message);
}

static void test_logger_arguments(void) {
  static const int ports[] = {-1, 0, 65536};
  lokiverse_logger *logger = NULL;
  size_t index;
  CHECK(make_http_logger("", 0, 3100, &logger) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(make_http_logger("local\0host", 10, 3100, &logger) ==
        LOKIVERSE_INVALID_ARGUMENT);
  for (index = 0; index < sizeof(ports) / sizeof(ports[0]); ++index) {
    CHECK(make_http_logger("localhost", 9, ports[index], &logger) ==
          LOKIVERSE_INVALID_ARGUMENT);
    CHECK(logger == NULL);
  }
  CHECK(make_http_logger("localhost", 9, 1, &logger) == LOKIVERSE_OK);
  CHECK(logger != NULL);
  lokiverse_logger_free(logger);
  CHECK(make_http_logger("localhost", 9, 65535, &logger) == LOKIVERSE_OK);
  lokiverse_logger_free(logger);
}

static void test_push_without_server(void) {
  lokiverse_message *message = sample();
  lokiverse_message *empty = lokiverse_message_new();
  lokiverse_logger *logger = NULL;
  int success = 1;
  const int port = closed_port();
  CHECK(port > 0);
  CHECK(make_http_logger("127.0.0.1", 9, port, &logger) == LOKIVERSE_OK);
  CHECK(lokiverse_logger_push(logger, empty, &success) == LOKIVERSE_OK);
  CHECK(success == 0);
  success = 1;
  CHECK(lokiverse_logger_push(logger, message, &success) == LOKIVERSE_OK);
  CHECK(success == 0);
  CHECK(lokiverse_logger_push(logger, NULL, &success) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_logger_push(logger, message, NULL) ==
        LOKIVERSE_INVALID_ARGUMENT);
  lokiverse_logger_free(logger);
  lokiverse_message_free(empty);
  lokiverse_message_free(message);
}

struct callback_state {
  int calls;
  int destroyed;
  const char *expected;
  size_t expected_size;
};

static int callback_post(void *context, const char *path, size_t path_size,
                         const char *body, size_t body_size,
                         const char *content_type, size_t content_type_size) {
  struct callback_state *state = context;
  CHECK(path_size == sizeof("/loki/api/v1/push") - 1 &&
        memcmp(path, "/loki/api/v1/push", path_size) == 0);
  CHECK(content_type_size == sizeof("application/x-protobuf") - 1 &&
        memcmp(content_type, "application/x-protobuf", content_type_size) == 0);
  CHECK(body_size == state->expected_size);
  CHECK(body_size == state->expected_size &&
        memcmp(body, state->expected, body_size) == 0);
  ++state->calls;
  return 204;
}

static void callback_destroy(void *context) {
  ++((struct callback_state *)context)->destroyed;
}

static int callback_push(void *context, const lokiverse_message *message,
                         int *success) {
  struct callback_state *state = context;
  char *body = NULL;
  size_t size = 0;
  const int status = lokiverse_message_encode(message, &body, &size);
  ++state->calls;
  *success = status == LOKIVERSE_OK && size == state->expected_size &&
             memcmp(body, state->expected, size) == 0;
  lokiverse_buffer_free(body);
  return LOKIVERSE_OK;
}

static void test_callbacks_and_lifetimes(void) {
  lokiverse_message *message = sample();
  char *body = NULL;
  size_t size = 0;
  int success = 0;
  int safe;
  CHECK(lokiverse_message_encode(message, &body, &size) == LOKIVERSE_OK);
  for (safe = 0; safe < 2; ++safe) {
    struct callback_state state = {0, 0, body, size};
    lokiverse_transport *transport = NULL;
    lokiverse_logger *logger = NULL;
    lokiverse_transport_callbacks callbacks = {callback_post, callback_destroy};
    CHECK(lokiverse_transport_make_custom(callbacks, &state, &transport) ==
          LOKIVERSE_OK);
    CHECK((safe ? lokiverse_logger_make_safe(transport, &logger)
                : lokiverse_logger_make_unsafe(transport, &logger)) ==
          LOKIVERSE_OK);
    lokiverse_transport_free(transport);
    CHECK(state.destroyed == 0);
    CHECK(lokiverse_logger_push(logger, message, &success) == LOKIVERSE_OK &&
          success == 1);
    CHECK(state.calls == 1);
    lokiverse_logger_free(logger);
    CHECK(state.destroyed == 1);
  }
  {
    struct callback_state state = {0, 0, body, size};
    lokiverse_logger *logger = NULL;
    lokiverse_logger_callbacks callbacks = {callback_push, callback_destroy};
    CHECK(lokiverse_logger_make_custom(callbacks, &state, &logger) ==
          LOKIVERSE_OK);
    CHECK(lokiverse_logger_push(logger, message, &success) == LOKIVERSE_OK &&
          success == 1);
    CHECK(state.calls == 1);
    lokiverse_logger_free(logger);
    CHECK(state.destroyed == 1);
  }
  lokiverse_buffer_free(body);
  lokiverse_message_free(message);
}

static void test_reserve_and_value(void) {
  lokiverse_message *message = lokiverse_message_new();
  lokiverse_stream *stream = lokiverse_message_stream(message);
  lokiverse_value value;
  CHECK(lokiverse_value_now("a\0b", 3, &value) == LOKIVERSE_OK);
  CHECK(value.timestamp_ns > 0 && value.line_size == 3 &&
        memcmp(value.line, "a\0b", 3) == 0);
  CHECK(lokiverse_stream_reserve(stream, 1, 1) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_label(stream, "job", 3, "kept", 4) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_entry(stream, value.timestamp_ns, value.line,
                               value.line_size) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_reserve(stream, 0, SIZE_MAX) ==
        LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_stream_reserve(stream, 0, 0) == LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_OK);
  lokiverse_value_free(&value);
  CHECK(value.line == NULL && value.line_size == 0);
  lokiverse_value_free(&value);
  lokiverse_message_free(message);
  CHECK(lokiverse_stream_entry_now(stream, "after parent free", 17) ==
        LOKIVERSE_OK);
  CHECK(lokiverse_stream_commit(stream) == LOKIVERSE_OK);
  lokiverse_stream_free(stream);
  CHECK(lokiverse_stream_reserve(NULL, 0, 0) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_value_now(NULL, 1, &value) == LOKIVERSE_INVALID_ARGUMENT);
  CHECK(lokiverse_value_now(NULL, 0, NULL) == LOKIVERSE_INVALID_ARGUMENT);
  lokiverse_value_free(NULL);
  {
    lokiverse_logger *logger = (lokiverse_logger *)1;
    lokiverse_transport *transport = (lokiverse_transport *)1;
    lokiverse_transport_callbacks callbacks = {NULL, NULL};
    lokiverse_logger_callbacks logger_callbacks = {NULL, NULL};
    int status = 204;
    CHECK(lokiverse_logger_make_safe(NULL, &logger) ==
              LOKIVERSE_INVALID_ARGUMENT &&
          logger == NULL);
    CHECK(lokiverse_logger_make_unsafe(NULL, &logger) ==
              LOKIVERSE_INVALID_ARGUMENT &&
          logger == NULL);
    CHECK(lokiverse_transport_make_custom(callbacks, NULL, &transport) ==
              LOKIVERSE_INVALID_ARGUMENT &&
          transport == NULL);
    CHECK(lokiverse_logger_make_custom(logger_callbacks, NULL, &logger) ==
              LOKIVERSE_INVALID_ARGUMENT &&
          logger == NULL);
    CHECK(lokiverse_transport_post(NULL, NULL, 0, NULL, 0, NULL, 0, &status) ==
              LOKIVERSE_INVALID_ARGUMENT &&
          status == -1);
    lokiverse_transport_free(NULL);
  }
}

int main(void) {
  test_callbacks_and_lifetimes();
  test_reserve_and_value();
  test_encode();
  test_empty_message();
  test_commit_requires_entries();
  test_null_arguments();
  test_logger_arguments();
  test_push_without_server();
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("lokiverse_c_test: all checks passed");
  return EXIT_SUCCESS;
}
