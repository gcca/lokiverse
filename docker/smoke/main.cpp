#include <cstdio>
#include <string>

#include <lokiverse.hpp>

int main(int argc, char **argv) {
  lokiverse::Message message;
  message.Stream().Label("job", "smoke").Entry("static").Commit();
  const auto body = message.Encode();
  if (!body || body->empty()) {
    return 1;
  }
  if (argc == 3) {
    auto transport =
        lokiverse::Transport::MakeHttp(argv[1], std::stoi(argv[2]));
    const int status = transport->Post(lokiverse::kPushPath, *body,
                                       lokiverse::kProtobufContentType);
    std::printf("%s:%s status %d\n", argv[1], argv[2], status);
    return status > 0 ? 0 : 3;
  }
  auto transport = lokiverse::Transport::MakeHttp("127.0.0.1", 9);
  auto logger = lokiverse::Logger::MakeSafe(*transport);
  if (logger->Push(message)) {
    return 2;
  }
  std::printf("encoded %zu bytes\n", body->size());
  return 0;
}
