#include "lokiverse.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "lokiverse.hpp"

struct lokiverse_message {
  std::shared_ptr<lokiverse::Message> message;
};

struct lokiverse_stream {
  std::shared_ptr<lokiverse::Message> message;
  lokiverse::Message::StreamBuilder builder;

  explicit lokiverse_stream(std::shared_ptr<lokiverse::Message> owner)
      : message(std::move(owner)), builder(message->Stream()) {}
};

struct lokiverse_transport {
  std::shared_ptr<lokiverse::Transport> transport;
};

struct lokiverse_logger {
  std::shared_ptr<lokiverse::Transport> transport;
  std::unique_ptr<lokiverse::Logger> logger;
  lokiverse_logger_callbacks callbacks{};
  void *context = nullptr;

  ~lokiverse_logger() {
    logger.reset();
    if (callbacks.destroy) {
      callbacks.destroy(context);
    }
  }
};

namespace {

int Translate() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc &) {
    return LOKIVERSE_NO_MEMORY;
  } catch (const std::invalid_argument &) {
    return LOKIVERSE_INVALID_ARGUMENT;
  } catch (const std::length_error &) {
    return LOKIVERSE_INVALID_ARGUMENT;
  } catch (...) {
    return LOKIVERSE_INTERNAL;
  }
}

bool Readable(const char *data, size_t size) { return data || size == 0; }

std::string_view View(const char *data, size_t size) {
  return size == 0 ? std::string_view("") : std::string_view(data, size);
}

std::string Copy(const char *data, size_t size) {
  return std::string(View(data, size));
}

char *Buffer(std::string_view text) {
  auto *buffer = static_cast<char *>(std::malloc(text.size() + 1));
  if (!buffer) {
    throw std::bad_alloc();
  }
  if (!text.empty()) {
    std::memcpy(buffer, text.data(), text.size());
  }
  buffer[text.size()] = '\0';
  return buffer;
}

class CallbackTransport final : public lokiverse::Transport {
public:
  CallbackTransport(lokiverse_transport_callbacks callbacks, void *context)
      : callbacks_(callbacks), context_(context) {}

  ~CallbackTransport() override {
    if (callbacks_.destroy) {
      callbacks_.destroy(context_);
    }
  }

  int Post(std::string_view path, std::string_view body,
           std::string_view content_type) override {
    return callbacks_.post(context_, path.data(), path.size(), body.data(),
                           body.size(), content_type.data(),
                           content_type.size());
  }

private:
  lokiverse_transport_callbacks callbacks_;
  void *context_;
};

int MakeLogger(lokiverse_transport *transport, lokiverse_logger **logger,
               bool safe) {
  if (logger) {
    *logger = nullptr;
  }
  if (!transport || !logger) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    auto result = std::make_unique<lokiverse_logger>();
    result->transport = transport->transport;
    result->logger = safe ? lokiverse::Logger::MakeSafe(*result->transport)
                          : lokiverse::Logger::MakeUnsafe(*result->transport);
    *logger = result.release();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

}

extern "C" {

lokiverse_message *lokiverse_message_new(void) {
  try {
    return new lokiverse_message{std::make_shared<lokiverse::Message>()};
  } catch (...) {
    return nullptr;
  }
}

void lokiverse_message_free(lokiverse_message *message) { delete message; }

lokiverse_stream *lokiverse_message_stream(lokiverse_message *message) {
  if (!message) {
    return nullptr;
  }
  try {
    return new lokiverse_stream(message->message);
  } catch (...) {
    return nullptr;
  }
}

int lokiverse_message_encode(const lokiverse_message *message, char **data,
                             size_t *size) {
  if (data) {
    *data = nullptr;
  }
  if (size) {
    *size = 0;
  }
  if (!message || !data || !size) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    const auto body = message->message->Encode();
    if (!body) {
      return LOKIVERSE_INVALID_ARGUMENT;
    }
    *data = Buffer(*body);
    *size = body->size();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

void lokiverse_buffer_free(char *data) { std::free(data); }

int lokiverse_value_now(const char *line, size_t line_size,
                        lokiverse_value *value) {
  if (!value) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  *value = {};
  if (!Readable(line, line_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    const auto now = lokiverse::Value::Now({});
    auto *buffer = Buffer(View(line, line_size));
    *value = {now.timestamp_ns, buffer, line_size};
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

void lokiverse_value_free(lokiverse_value *value) {
  if (value) {
    std::free(value->line);
    *value = {};
  }
}

int lokiverse_stream_reserve(lokiverse_stream *stream, size_t label_count,
                             size_t value_count) {
  if (!stream) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    stream->builder.Reserve(label_count, value_count);
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_stream_label(lokiverse_stream *stream, const char *name,
                           size_t name_size, const char *value,
                           size_t value_size) {
  if (!stream || !Readable(name, name_size) || !Readable(value, value_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    stream->builder.Label(Copy(name, name_size), Copy(value, value_size));
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_stream_entry(lokiverse_stream *stream, uint64_t timestamp_ns,
                           const char *line, size_t line_size) {
  if (!stream || !Readable(line, line_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    stream->builder.Entry(timestamp_ns, Copy(line, line_size));
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_stream_entry_now(lokiverse_stream *stream, const char *line,
                               size_t line_size) {
  if (!stream || !Readable(line, line_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    stream->builder.Entry(Copy(line, line_size));
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_stream_commit(lokiverse_stream *stream) {
  if (!stream) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    stream->builder.Commit();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

void lokiverse_stream_free(lokiverse_stream *stream) { delete stream; }

int lokiverse_transport_make_http(const char *host, size_t host_size, int port,
                                  lokiverse_transport **transport) {
  if (transport) {
    *transport = nullptr;
  }
  if (!transport || !host || host_size == 0 || port < 1 || port > 65535 ||
      std::memchr(host, '\0', host_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    auto result = std::make_unique<lokiverse_transport>();
    result->transport =
        lokiverse::Transport::MakeHttp(Copy(host, host_size), port);
    *transport = result.release();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_transport_make_custom(lokiverse_transport_callbacks callbacks,
                                    void *context,
                                    lokiverse_transport **transport) {
  if (transport) {
    *transport = nullptr;
  }
  if (!transport || !callbacks.post) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    auto result = std::make_unique<lokiverse_transport>();
    result->transport = std::make_shared<CallbackTransport>(callbacks, context);
    *transport = result.release();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_transport_post(lokiverse_transport *transport, const char *path,
                             size_t path_size, const char *body,
                             size_t body_size, const char *content_type,
                             size_t content_type_size, int *status) {
  if (status) {
    *status = -1;
  }
  if (!transport || !status || !Readable(path, path_size) ||
      !Readable(body, body_size) ||
      !Readable(content_type, content_type_size)) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    *status =
        transport->transport->Post(View(path, path_size), View(body, body_size),
                                   View(content_type, content_type_size));
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

void lokiverse_transport_free(lokiverse_transport *transport) {
  delete transport;
}

int lokiverse_logger_make_safe(lokiverse_transport *transport,
                               lokiverse_logger **logger) {
  return MakeLogger(transport, logger, true);
}

int lokiverse_logger_make_unsafe(lokiverse_transport *transport,
                                 lokiverse_logger **logger) {
  return MakeLogger(transport, logger, false);
}

int lokiverse_logger_make_custom(lokiverse_logger_callbacks callbacks,
                                 void *context, lokiverse_logger **logger) {
  if (logger) {
    *logger = nullptr;
  }
  if (!logger || !callbacks.push) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    auto result = std::make_unique<lokiverse_logger>();
    result->callbacks = callbacks;
    result->context = context;
    *logger = result.release();
    return LOKIVERSE_OK;
  } catch (...) {
    return Translate();
  }
}

int lokiverse_logger_push(lokiverse_logger *logger,
                          const lokiverse_message *message, int *success) {
  if (success) {
    *success = 0;
  }
  if (!logger || !message || !success) {
    return LOKIVERSE_INVALID_ARGUMENT;
  }
  try {
    if (logger->callbacks.push) {
      const int result =
          logger->callbacks.push(logger->context, message, success);
      if (result != LOKIVERSE_OK) {
        *success = 0;
      }
      return result;
    }
    *success = logger->logger->Push(*message->message) ? 1 : 0;
    return LOKIVERSE_OK;
  } catch (...) {
    *success = 0;
    return Translate();
  }
}

void lokiverse_logger_free(lokiverse_logger *logger) { delete logger; }
}
