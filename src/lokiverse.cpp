#include "lokiverse.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>

#include <httplib.h>
#include <snappy.h>

#include "push.pb.h"

namespace lokiverse {

namespace {

class HttpTransport final : public Transport {
public:
  HttpTransport(const std::string &host, int port) : client_(host, port) {}

  int Post(std::string_view path, std::string_view body,
           std::string_view content_type) override {
    auto res = client_.Post(std::string(path), body.data(), body.size(),
                            std::string(content_type));
    return res ? res->status : -1;
  }

private:
  httplib::Client client_;
};

bool PostBody(Transport &transport, const std::string &body) {
  return transport.Post(kPushPath, body, kProtobufContentType) == 204;
}

class UnsafeLogger final : public Logger {
public:
  explicit UnsafeLogger(Transport &transport) : transport_(transport) {}

  bool Push(const Message &message) override {
    const auto body = message.Encode();
    if (!body) {
      return false;
    }
    return PostBody(transport_, *body);
  }

private:
  Transport &transport_;
};

class SafeLogger final : public Logger {
public:
  explicit SafeLogger(Transport &transport) : transport_(transport) {}

  bool Push(const Message &message) override {
    const auto body = message.Encode();
    if (!body) {
      return false;
    }
    std::lock_guard lock(mutex_);
    return PostBody(transport_, *body);
  }

private:
  Transport &transport_;
  std::mutex mutex_;
};

bool IsIdentifier(std::string_view name) {
  const auto letter = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (name.empty() || !letter(name.front())) {
    return false;
  }
  return std::all_of(name.begin() + 1, name.end(), [&](unsigned char c) {
    return letter(c) || (c >= '0' && c <= '9');
  });
}

void AppendQuoted(std::string &result, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  result += '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += c;
    } else if (c < 0x20 || c == 0x7f) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += c;
    }
  }
  result += '"';
}

std::string EncodeLabels(const Message::Labels &labels) {
  std::string result = "{";
  for (const auto &[key, value] : labels) {
    if (result.size() > 1) {
      result += ',';
    }
    if (IsIdentifier(key)) {
      result += key;
    } else {
      AppendQuoted(result, key);
    }
    result += '=';
    AppendQuoted(result, value);
  }
  result += '}';
  return result;
}

}

std::unique_ptr<Transport> Transport::MakeHttp(const std::string &host,
                                               int port) {
  return std::make_unique<HttpTransport>(host, port);
}

Value Value::Now(std::string line) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto timestamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return {static_cast<std::uint64_t>(timestamp), std::move(line)};
}

Message::StreamBuilder::StreamBuilder(Message &message) : message_(message) {}

Message::StreamBuilder &
Message::StreamBuilder::Reserve(std::size_t label_count,
                                std::size_t value_count) {
  labels_.reserve(label_count);
  values_.reserve(value_count);
  return *this;
}

Message::StreamBuilder &Message::StreamBuilder::Label(std::string name,
                                                      std::string value) {
  labels_.emplace_back(std::move(name), std::move(value));
  return *this;
}

Message::StreamBuilder &
Message::StreamBuilder::Entry(std::uint64_t timestamp_ns, std::string line) {
  values_.emplace_back(timestamp_ns, std::move(line));
  return *this;
}

Message::StreamBuilder &Message::StreamBuilder::Entry(std::string line) {
  values_.push_back(Value::Now(std::move(line)));
  return *this;
}

void Message::StreamBuilder::Commit() {
  if (values_.empty()) {
    throw std::invalid_argument("values must not be empty");
  }
  std::stable_sort(
      labels_.begin(), labels_.end(),
      [](const auto &a, const auto &b) { return a.first < b.first; });
  message_.streams_.emplace_back(std::move(labels_), std::move(values_));
  labels_.clear();
  values_.clear();
}

Message::StreamBuilder Message::Stream() { return StreamBuilder(*this); }

std::optional<std::string> Message::Encode() const {
  if (streams_.empty()) {
    return std::nullopt;
  }
  wire::PushRequest request;
  for (const auto &stream : streams_) {
    auto *output = request.add_streams();
    output->set_labels(EncodeLabels(stream.labels));
    for (const auto &value : stream.values) {
      auto *entry = output->add_entries();
      auto *timestamp = entry->mutable_timestamp();
      timestamp->set_seconds(
          static_cast<std::int64_t>(value.timestamp_ns / 1000000000));
      timestamp->set_nanos(
          static_cast<std::int32_t>(value.timestamp_ns % 1000000000));
      entry->set_line(value.line);
    }
  }
  std::string serialized;
  if (!request.SerializeToString(&serialized)) {
    return std::nullopt;
  }
  std::string compressed;
  snappy::Compress(serialized.data(), serialized.size(), &compressed);
  return compressed;
}

std::unique_ptr<Logger> Logger::MakeSafe(Transport &transport) {
  return std::make_unique<SafeLogger>(transport);
}

std::unique_ptr<Logger> Logger::MakeUnsafe(Transport &transport) {
  return std::make_unique<UnsafeLogger>(transport);
}

}
