#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lokiverse {

inline constexpr std::string_view kPushPath = "/loki/api/v1/push";
inline constexpr std::string_view kProtobufContentType =
    "application/x-protobuf";

class Transport {
public:
  Transport() = default;
  virtual ~Transport() = default;

  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;
  Transport(Transport &&) = delete;
  Transport &operator=(Transport &&) = delete;

  static std::unique_ptr<Transport> MakeHttp(const std::string &host, int port);

  virtual int Post(std::string_view path, std::string_view body,
                   std::string_view content_type) = 0;
};

struct Value {
  std::uint64_t timestamp_ns;
  std::string line;

  static Value Now(std::string line);
};

class Message {
public:
  using Labels = std::vector<std::pair<std::string, std::string>>;

  class StreamBuilder {
  public:
    StreamBuilder(const StreamBuilder &) = delete;
    StreamBuilder &operator=(const StreamBuilder &) = delete;
    StreamBuilder(StreamBuilder &&) = delete;
    StreamBuilder &operator=(StreamBuilder &&) = delete;

    StreamBuilder &Reserve(std::size_t label_count, std::size_t value_count);
    StreamBuilder &Label(std::string name, std::string value);
    StreamBuilder &Entry(std::uint64_t timestamp_ns, std::string line);
    StreamBuilder &Entry(std::string line);
    void Commit();

  private:
    explicit StreamBuilder(Message &message);

    Message &message_;
    Labels labels_;
    std::vector<Value> values_;

    friend class Message;
  };

  [[nodiscard]] StreamBuilder Stream();

  std::optional<std::string> Encode() const;

private:
  struct Record {
    Labels labels;
    std::vector<Value> values;
  };

  std::vector<Record> streams_;
};

class Logger {
public:
  Logger() = default;
  virtual ~Logger() = default;

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;
  Logger(Logger &&) = delete;
  Logger &operator=(Logger &&) = delete;

  static std::unique_ptr<Logger> MakeSafe(Transport &transport);
  static std::unique_ptr<Logger> MakeUnsafe(Transport &transport);

  virtual bool Push(const Message &message) = 0;
};

}
