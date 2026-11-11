#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <snappy.h>

#include "lokiverse.hpp"
#include "push.pb.h"

namespace {

using ::testing::_;
using ::testing::Eq;
using ::testing::Return;
using ::testing::StrictMock;

class MockTransport : public lokiverse::Transport {
public:
  MOCK_METHOD(int, Post,
              (std::string_view path, std::string_view body,
               std::string_view content_type),
              (override));
};

lokiverse::wire::PushRequest Decode(std::string_view compressed) {
  std::string serialized;
  EXPECT_TRUE(
      snappy::Uncompress(compressed.data(), compressed.size(), &serialized));
  lokiverse::wire::PushRequest request;
  EXPECT_TRUE(request.ParseFromString(serialized));
  return request;
}

lokiverse::wire::PushRequest Capture(MockTransport &transport,
                                     lokiverse::Logger &logger,
                                     const lokiverse::Message &message) {
  std::string body;
  EXPECT_CALL(transport,
              Post(Eq("/loki/api/v1/push"), _, Eq("application/x-protobuf")))
      .WillOnce([&](std::string_view, std::string_view data, std::string_view) {
        body.assign(data);
        return 204;
      });
  EXPECT_TRUE(logger.Push(message));
  return Decode(body);
}

using Factory = std::unique_ptr<lokiverse::Logger> (*)(lokiverse::Transport &);

class LoggerTest : public ::testing::TestWithParam<Factory> {};

std::uint64_t EpochNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TEST_P(LoggerTest, PushesMultipleStreamsAndEntries) {
  MockTransport transport;
  auto logger = GetParam()(transport);
  lokiverse::Message message;
  message.Stream()
      .Label("job", "api")
      .Label("env", "test")
      .Entry(1700000000123456789ULL, "first")
      .Entry(1700000000123456790ULL, "second")
      .Commit();
  message.Stream()
      .Label("job", "worker")
      .Entry(1700000001123456789ULL, "third")
      .Commit();

  const auto request = Capture(transport, *logger, message);
  ASSERT_EQ(request.streams_size(), 2);
  EXPECT_EQ(request.streams(0).labels(), "{env=\"test\",job=\"api\"}");
  ASSERT_EQ(request.streams(0).entries_size(), 2);
  const auto &first = request.streams(0).entries(0);
  EXPECT_EQ(first.timestamp().seconds(), 1700000000);
  EXPECT_EQ(first.timestamp().nanos(), 123456789);
  EXPECT_EQ(first.line(), "first");
  EXPECT_EQ(request.streams(0).entries(1).timestamp().nanos(), 123456790);
  EXPECT_EQ(request.streams(1).labels(), "{job=\"worker\"}");
  ASSERT_EQ(request.streams(1).entries_size(), 1);
  EXPECT_EQ(request.streams(1).entries(0).timestamp().seconds(), 1700000001);
  EXPECT_EQ(request.streams(1).entries(0).line(), "third");
}

TEST_P(LoggerTest, PreservesTimestampBoundariesAndEmptyLines) {
  MockTransport transport;
  auto logger = GetParam()(transport);
  lokiverse::Message message;
  const std::vector<std::uint64_t> timestamps = {
      0, 1, 999999999, 1000000000, 1700000000123456789ULL, UINT64_MAX};
  auto stream = message.Stream();
  stream.Label("job", "test");
  for (auto timestamp : timestamps) {
    stream.Entry(timestamp, "");
  }
  stream.Commit();

  const auto request = Capture(transport, *logger, message);
  const auto &output = request.streams(0);
  ASSERT_EQ(output.entries_size(), timestamps.size());
  for (std::size_t i = 0; i < timestamps.size(); ++i) {
    const auto &entry = output.entries(static_cast<int>(i));
    EXPECT_TRUE(entry.has_timestamp());
    EXPECT_EQ(static_cast<std::uint64_t>(entry.timestamp().seconds()) *
                      1000000000 +
                  entry.timestamp().nanos(),
              timestamps[i]);
    EXPECT_GE(entry.timestamp().nanos(), 0);
    EXPECT_LT(entry.timestamp().nanos(), 1000000000);
    EXPECT_TRUE(entry.line().empty());
  }
}

TEST_P(LoggerTest, EscapesLabelsAndPreservesBinaryLengths) {
  MockTransport transport;
  auto logger = GetParam()(transport);
  lokiverse::Message message;
  const std::string text = "quote\"\\\n" + std::string(1, '\0') + "café";
  message.Stream().Label("clé", text).Entry(1, std::string("a\0b", 3)).Commit();

  const auto request = Capture(transport, *logger, message);
  EXPECT_EQ(request.streams(0).labels(),
            "{\"clé\"=\"quote\\\"\\\\\\u000a\\u0000café\"}");
  EXPECT_EQ(request.streams(0).entries(0).line(), std::string("a\0b", 3));
}

TEST(MessageTest, OwnsItsInputsAndCopiesIndependently) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  std::string name = "job";
  std::string value = "original";
  std::string line = "original";
  lokiverse::Message message;
  message.Stream().Label(name, value).Entry(1, line).Commit();
  name = "changed";
  value = "changed";
  line = "changed";
  const auto snapshot = message;
  message.Stream().Label("job", "later").Entry(2, "later").Commit();

  const auto request = Capture(transport, *logger, snapshot);
  ASSERT_EQ(request.streams_size(), 1);
  EXPECT_EQ(request.streams(0).labels(), "{job=\"original\"}");
  EXPECT_EQ(request.streams(0).entries(0).line(), "original");
  EXPECT_EQ(Capture(transport, *logger, message).streams_size(), 2);
}

TEST(MessageTest, PreservesEntryOrderAndCanBePushedAgain) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  message.Stream()
      .Label("job", "test")
      .Entry(2, "second")
      .Entry(1, "first")
      .Entry(1, "again")
      .Commit();
  const auto first = Capture(transport, *logger, message);
  const auto second = Capture(transport, *logger, message);
  EXPECT_EQ(first.SerializeAsString(), second.SerializeAsString());
  EXPECT_EQ(first.streams(0).entries(0).line(), "second");
  EXPECT_EQ(first.streams(0).entries(1).line(), "first");
  EXPECT_EQ(first.streams(0).entries(2).line(), "again");
}

TEST(MessageTest, RejectsEmptyValuesWithoutAddingAStream) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  EXPECT_THROW(message.Stream().Label("job", "test").Commit(),
               std::invalid_argument);
  message.Stream().Label("job", "valid").Entry(1, "valid").Commit();
  EXPECT_EQ(Capture(transport, *logger, message).streams_size(), 1);
}

TEST_P(LoggerTest, EmptyMessageDoesNotPost) {
  StrictMock<MockTransport> transport;
  auto logger = GetParam()(transport);
  EXPECT_FALSE(logger->Push(lokiverse::Message{}));
}

TEST_P(LoggerTest, ReturnValueFollowsStatus) {
  MockTransport transport;
  auto logger = GetParam()(transport);
  lokiverse::Message message;
  message.Stream().Label("job", "test").Entry(1, "hello").Commit();
  EXPECT_CALL(transport, Post(_, _, _))
      .WillOnce(Return(200))
      .WillOnce(Return(204))
      .WillOnce(Return(400))
      .WillOnce(Return(500))
      .WillOnce(Return(-1));
  EXPECT_FALSE(logger->Push(message));
  EXPECT_TRUE(logger->Push(message));
  EXPECT_FALSE(logger->Push(message));
  EXPECT_FALSE(logger->Push(message));
  EXPECT_FALSE(logger->Push(message));
}

INSTANTIATE_TEST_SUITE_P(Loggers, LoggerTest,
                         ::testing::Values(&lokiverse::Logger::MakeUnsafe,
                                           &lokiverse::Logger::MakeSafe),
                         [](const ::testing::TestParamInfo<Factory> &info) {
                           return std::string(
                               info.param == &lokiverse::Logger::MakeSafe
                                   ? "Safe"
                                   : "Unsafe");
                         });

TEST(ValueTest, NowCapturesWallClockAndOwnsLine) {
  const auto before = EpochNs();
  const auto value = lokiverse::Value::Now(std::string("a\0b", 3));
  const auto after = EpochNs();
  EXPECT_LE(before, value.timestamp_ns);
  EXPECT_LE(value.timestamp_ns, after);
  EXPECT_EQ(value.line, std::string("a\0b", 3));

  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  message.Stream()
      .Label("job", "now")
      .Entry(value.timestamp_ns, value.line)
      .Commit();
  const auto request = Capture(transport, *logger, message);
  const auto &entry = request.streams(0).entries(0);
  EXPECT_EQ(static_cast<std::uint64_t>(entry.timestamp().seconds()),
            value.timestamp_ns / 1000000000);
  EXPECT_EQ(static_cast<std::uint64_t>(entry.timestamp().nanos()),
            value.timestamp_ns % 1000000000);
  EXPECT_EQ(entry.line(), std::string("a\0b", 3));
}

TEST(StreamBuilderTest, EntryWithoutTimestampUsesNow) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  const auto before = EpochNs();
  message.Stream().Label("job", "now").Entry(std::string("a\0b", 3)).Commit();
  const auto after = EpochNs();

  const auto request = Capture(transport, *logger, message);
  const auto &entry = request.streams(0).entries(0);
  const auto timestamp =
      static_cast<std::uint64_t>(entry.timestamp().seconds()) * 1000000000 +
      static_cast<std::uint64_t>(entry.timestamp().nanos());
  EXPECT_LE(before, timestamp);
  EXPECT_LE(timestamp, after);
  EXPECT_EQ(entry.line(), std::string("a\0b", 3));
}

TEST(StreamBuilderTest, UncommittedBuilderAddsNothing) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  {
    auto stream = message.Stream();
    stream.Label("job", "dropped").Entry(1, "dropped");
  }
  EXPECT_CALL(transport, Post(_, _, _)).Times(0);
  EXPECT_FALSE(logger->Push(message));
}

TEST(StreamBuilderTest, CommitResetsBuilder) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  auto stream = message.Stream();
  stream.Label("job", "once").Entry(1, "once").Commit();
  EXPECT_THROW(stream.Commit(), std::invalid_argument);
  stream.Label("job", "twice").Entry(2, "twice").Commit();

  const auto request = Capture(transport, *logger, message);
  ASSERT_EQ(request.streams_size(), 2);
  EXPECT_EQ(request.streams(0).labels(), "{job=\"once\"}");
  EXPECT_EQ(request.streams(1).labels(), "{job=\"twice\"}");
  EXPECT_EQ(request.streams(1).entries_size(), 1);
}

TEST(MessageTest, EncodeReturnsPostedBody) {
  MockTransport transport;
  auto logger = lokiverse::Logger::MakeUnsafe(transport);
  lokiverse::Message message;
  message.Stream().Label("job", "encode").Entry(1, "body").Commit();
  const auto encoded = message.Encode();
  ASSERT_TRUE(encoded.has_value());

  std::string body;
  EXPECT_CALL(transport, Post(_, _, _))
      .WillOnce([&](std::string_view, std::string_view data, std::string_view) {
        body.assign(data);
        return 204;
      });
  EXPECT_TRUE(logger->Push(message));
  EXPECT_EQ(*encoded, body);
  const auto request = Decode(*encoded);
  ASSERT_EQ(request.streams_size(), 1);
  EXPECT_EQ(request.streams(0).labels(), "{job=\"encode\"}");
  EXPECT_EQ(request.streams(0).entries(0).line(), "body");
}

TEST(MessageTest, EncodeEmptyMessageReturnsNullopt) {
  EXPECT_FALSE(lokiverse::Message{}.Encode().has_value());
}

class CountingTransport : public lokiverse::Transport {
public:
  int Post(std::string_view, std::string_view body, std::string_view) override {
    const int current = in_flight_.fetch_add(1) + 1;
    int seen = max_in_flight_.load();
    while (current > seen &&
           !max_in_flight_.compare_exchange_weak(seen, current)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
      std::lock_guard lock(mutex_);
      bodies_.emplace_back(body);
    }
    in_flight_.fetch_sub(1);
    return 204;
  }

  int MaxInFlight() const { return max_in_flight_.load(); }

  std::vector<std::string> Bodies() {
    std::lock_guard lock(mutex_);
    return bodies_;
  }

private:
  std::atomic<int> in_flight_{0};
  std::atomic<int> max_in_flight_{0};
  std::mutex mutex_;
  std::vector<std::string> bodies_;
};

TEST(SafeLoggerTest, SerializesConcurrentPushes) {
  CountingTransport transport;
  auto logger = lokiverse::Logger::MakeSafe(transport);
  lokiverse::Message built;
  built.Stream().Label("job", "threads").Entry(1, "shared").Commit();
  const lokiverse::Message &message = built;
  const auto expected = message.Encode();
  ASSERT_TRUE(expected.has_value());

  constexpr int kThreads = 8;
  constexpr int kPushes = 25;
  std::atomic<int> successes{0};
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < kPushes; ++i) {
          if (logger->Push(message)) {
            successes.fetch_add(1);
          }
        }
      });
    }
  }

  EXPECT_EQ(successes.load(), kThreads * kPushes);
  EXPECT_EQ(transport.MaxInFlight(), 1);
  const auto bodies = transport.Bodies();
  ASSERT_EQ(bodies.size(), static_cast<std::size_t>(kThreads * kPushes));
  EXPECT_TRUE(std::ranges::all_of(
      bodies, [&](const std::string &body) { return body == *expected; }));
}

TEST(StreamBuilderTest, FailedCommitKeepsBuilderState) {
  lokiverse::Message message;
  auto stream = message.Stream();
  stream.Label("job", "kept");
  EXPECT_THROW(stream.Commit(), std::invalid_argument);
  EXPECT_FALSE(message.Encode().has_value());
  stream.Entry(1, "after").Commit();

  const auto request = Decode(*message.Encode());
  ASSERT_EQ(request.streams_size(), 1);
  EXPECT_EQ(request.streams(0).labels(), "{job=\"kept\"}");
  EXPECT_EQ(request.streams(0).entries(0).line(), "after");
}

TEST(MessageTest, EqualLabelSetsStaySeparateStreams) {
  lokiverse::Message message;
  message.Stream().Label("job", "same").Entry(2, "first").Commit();
  message.Stream().Label("job", "same").Entry(1, "second").Commit();

  const auto request = Decode(*message.Encode());
  ASSERT_EQ(request.streams_size(), 2);
  EXPECT_EQ(request.streams(0).labels(), request.streams(1).labels());
  EXPECT_EQ(request.streams(0).entries(0).line(), "first");
  EXPECT_EQ(request.streams(1).entries(0).line(), "second");
}

TEST(LabelEncodingTest, QuotesNonIdentifierNames) {
  lokiverse::Message message;
  message.Stream()
      .Label("1a", "v")
      .Label("a-b", "v")
      .Label("a.b", "v")
      .Label("", "v")
      .Label("_a", "v")
      .Label("a1", "v")
      .Entry(1, "x")
      .Commit();

  EXPECT_EQ(Decode(*message.Encode()).streams(0).labels(),
            "{\"\"=\"v\",\"1a\"=\"v\",_a=\"v\",\"a-b\"=\"v\",\"a.b\"=\"v\","
            "a1=\"v\"}");
}

TEST(LabelEncodingTest, EscapesControlBoundaries) {
  lokiverse::Message message;
  message.Stream().Label("k", "\x1f \x7f~").Entry(1, "x").Commit();

  EXPECT_EQ(Decode(*message.Encode()).streams(0).labels(),
            "{k=\"\\u001f \\u007f~\"}");
}

TEST(LabelEncodingTest, EmptyLabelSetEncodesBraces) {
  lokiverse::Message message;
  message.Stream().Entry(1, "x").Commit();

  EXPECT_EQ(Decode(*message.Encode()).streams(0).labels(), "{}");
}

TEST(LabelEncodingTest, SortsBytewiseAndKeepsDuplicateOrder) {
  lokiverse::Message message;
  message.Stream()
      .Label("b", "1")
      .Label("a", "2")
      .Label("Z", "3")
      .Label("a", "1")
      .Entry(1, "x")
      .Commit();

  EXPECT_EQ(Decode(*message.Encode()).streams(0).labels(),
            "{Z=\"3\",a=\"2\",a=\"1\",b=\"1\"}");
}

struct Received {
  std::string method;
  std::string path;
  std::string content_type;
  std::string body;
};

class LoopbackServer {
public:
  explicit LoopbackServer(int status) {
    server_.Post(".*", [this, status](const httplib::Request &request,
                                      httplib::Response &response) {
      std::lock_guard lock(mutex_);
      received_.push_back({request.method, request.path,
                           request.get_header_value("Content-Type"),
                           request.body});
      response.status = status;
    });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::jthread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  LoopbackServer(const LoopbackServer &) = delete;
  LoopbackServer &operator=(const LoopbackServer &) = delete;

  ~LoopbackServer() { server_.stop(); }

  int Port() const { return port_; }

  std::vector<Received> Requests() {
    std::lock_guard lock(mutex_);
    return received_;
  }

private:
  httplib::Server server_;
  std::mutex mutex_;
  std::vector<Received> received_;
  int port_ = -1;
  std::jthread thread_;
};

TEST(HttpTransportTest, PostsProtobufBodyToPushPath) {
  LoopbackServer server(204);
  ASSERT_GT(server.Port(), 0);
  auto transport = lokiverse::Transport::MakeHttp("127.0.0.1", server.Port());
  auto logger = lokiverse::Logger::MakeSafe(*transport);
  lokiverse::Message message;
  message.Stream().Label("job", "http").Entry(1, "over the wire").Commit();

  EXPECT_TRUE(logger->Push(message));
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 1);
  EXPECT_EQ(requests[0].method, "POST");
  EXPECT_EQ(requests[0].path, "/loki/api/v1/push");
  EXPECT_EQ(requests[0].content_type, "application/x-protobuf");
  EXPECT_EQ(requests[0].body, *message.Encode());
}

TEST(HttpTransportTest, ReturnsServerStatus) {
  for (const int status : {200, 204, 500}) {
    LoopbackServer server(status);
    ASSERT_GT(server.Port(), 0);
    auto transport = lokiverse::Transport::MakeHttp("127.0.0.1", server.Port());
    EXPECT_EQ(
        transport->Post("/loki/api/v1/push", "body", "application/x-protobuf"),
        status);
  }
}

TEST(HttpTransportTest, ConnectionFailureReturnsMinusOne) {
  int port;
  {
    LoopbackServer closed(204);
    port = closed.Port();
  }
  ASSERT_GT(port, 0);
  auto transport = lokiverse::Transport::MakeHttp("127.0.0.1", port);
  EXPECT_EQ(
      transport->Post("/loki/api/v1/push", "body", "application/x-protobuf"),
      -1);
  auto logger = lokiverse::Logger::MakeUnsafe(*transport);
  lokiverse::Message message;
  message.Stream().Label("job", "down").Entry(1, "lost").Commit();
  EXPECT_FALSE(logger->Push(message));
}

}

TEST(StreamBuilderTest, ReservePreservesContentsAndCommitSemantics) {
  lokiverse::Message message;
  auto stream = message.Stream();
  EXPECT_EQ(&stream.Reserve(0, 0), &stream);
  stream.Reserve(1, 1).Label("b", "first").Entry(2, "second");
  stream.Reserve(8, 256)
      .Reserve(0, 0)
      .Label("a", "value")
      .Entry(1, "first")
      .Commit();
  const auto request = Decode(*message.Encode());
  ASSERT_EQ(request.streams_size(), 1);
  EXPECT_EQ(request.streams(0).labels(), "{a=\"value\",b=\"first\"}");
  ASSERT_EQ(request.streams(0).entries_size(), 2);
  EXPECT_EQ(request.streams(0).entries(0).line(), "second");
  EXPECT_EQ(request.streams(0).entries(1).line(), "first");
  stream.Reserve(1, 1).Label("job", "again");
  EXPECT_THROW(stream.Commit(), std::invalid_argument);
  stream.Entry(3, "third").Commit();
  EXPECT_EQ(Decode(*message.Encode()).streams_size(), 2);
}

TEST(StreamBuilderTest, ExcessiveReserveKeepsExistingValues) {
  lokiverse::Message message;
  auto stream = message.Stream();
  stream.Label("job", "kept").Entry(1, "kept");
  EXPECT_THROW(stream.Reserve(0, static_cast<std::size_t>(-1)),
               std::length_error);
  stream.Commit();
  const auto request = Decode(*message.Encode());
  EXPECT_EQ(request.streams(0).labels(), "{job=\"kept\"}");
  EXPECT_EQ(request.streams(0).entries(0).line(), "kept");
}
