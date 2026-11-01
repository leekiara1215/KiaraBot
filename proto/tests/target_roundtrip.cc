#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const auto* descriptor =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(argv[1]);
  if (descriptor == nullptr) {
    return 3;
  }
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(descriptor);
  if (prototype == nullptr) {
    return 4;
  }
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const std::string input(std::istreambuf_iterator<char>(std::cin), {});
  if (!message->ParseFromString(input)) {
    return 5;
  }
  std::string output;
  if (!message->SerializeToString(&output)) {
    return 6;
  }
  std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
  return std::cout.good() ? 0 : 7;
}
