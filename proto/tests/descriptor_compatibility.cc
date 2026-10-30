#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <google/protobuf/descriptor.pb.h>

namespace {

using google::protobuf::DescriptorProto;
using google::protobuf::EnumDescriptorProto;
using google::protobuf::FieldDescriptorProto;
using google::protobuf::FileDescriptorProto;
using google::protobuf::FileDescriptorSet;
using google::protobuf::MethodDescriptorProto;
using google::protobuf::ServiceDescriptorProto;

bool fail(std::string_view path, std::string_view message) {
  std::cerr << path << ": " << message << '\n';
  return false;
}

const DescriptorProto* find_message(const FileDescriptorProto& file, std::string_view name) {
  for (const DescriptorProto& message : file.message_type()) {
    if (message.name() == name) {
      return &message;
    }
  }
  return nullptr;
}

const DescriptorProto* find_nested_message(const DescriptorProto& message, std::string_view name) {
  for (const DescriptorProto& nested : message.nested_type()) {
    if (nested.name() == name) {
      return &nested;
    }
  }
  return nullptr;
}

const EnumDescriptorProto* find_enum(const FileDescriptorProto& file, std::string_view name) {
  for (const EnumDescriptorProto& enumeration : file.enum_type()) {
    if (enumeration.name() == name) {
      return &enumeration;
    }
  }
  return nullptr;
}

const EnumDescriptorProto* find_nested_enum(const DescriptorProto& message, std::string_view name) {
  for (const EnumDescriptorProto& enumeration : message.enum_type()) {
    if (enumeration.name() == name) {
      return &enumeration;
    }
  }
  return nullptr;
}

const FieldDescriptorProto* find_field_by_number(const DescriptorProto& message, int number) {
  for (const FieldDescriptorProto& field : message.field()) {
    if (field.number() == number) {
      return &field;
    }
  }
  return nullptr;
}

bool compare_enum(const EnumDescriptorProto& old_enum, const EnumDescriptorProto& current_enum,
                  const std::string& path) {
  for (const auto& old_value : old_enum.value()) {
    const auto current = std::find_if(
        current_enum.value().begin(), current_enum.value().end(),
        [&old_value](const auto& value) { return value.number() == old_value.number(); });
    if (current == current_enum.value().end()) {
      return fail(path, "enum value number was removed: " + std::to_string(old_value.number()));
    }
    if (current->name() != old_value.name()) {
      return fail(path, "enum value number changed name: " + std::to_string(old_value.number()));
    }
  }
  return true;
}

bool compare_message(const DescriptorProto& old_message, const DescriptorProto& current_message,
                     const std::string& path) {
  for (const FieldDescriptorProto& old_field : old_message.field()) {
    const FieldDescriptorProto* current_field =
        find_field_by_number(current_message, old_field.number());
    if (current_field == nullptr) {
      return fail(path, "field number was removed: " + std::to_string(old_field.number()));
    }
    if (current_field->name() != old_field.name() || current_field->type() != old_field.type() ||
        current_field->label() != old_field.label() ||
        current_field->type_name() != old_field.type_name() ||
        current_field->has_oneof_index() != old_field.has_oneof_index() ||
        current_field->oneof_index() != old_field.oneof_index()) {
      return fail(path, "field definition changed: " + old_field.name());
    }
  }

  for (const DescriptorProto& old_nested : old_message.nested_type()) {
    const DescriptorProto* current_nested = find_nested_message(current_message, old_nested.name());
    if (current_nested == nullptr) {
      return fail(path, "nested message was removed: " + old_nested.name());
    }
    if (!compare_message(old_nested, *current_nested, path + "." + old_nested.name())) {
      return false;
    }
  }

  for (const EnumDescriptorProto& old_enum : old_message.enum_type()) {
    const EnumDescriptorProto* current_enum = find_nested_enum(current_message, old_enum.name());
    if (current_enum == nullptr) {
      return fail(path, "nested enum was removed: " + old_enum.name());
    }
    if (!compare_enum(old_enum, *current_enum, path + "." + old_enum.name())) {
      return false;
    }
  }
  return true;
}

const FileDescriptorProto* find_file(const FileDescriptorSet& set, std::string_view name) {
  for (const FileDescriptorProto& file : set.file()) {
    if (file.name() == name) {
      return &file;
    }
  }
  return nullptr;
}

const ServiceDescriptorProto* find_service(const FileDescriptorProto& file, std::string_view name) {
  for (const ServiceDescriptorProto& service : file.service()) {
    if (service.name() == name) {
      return &service;
    }
  }
  return nullptr;
}

const MethodDescriptorProto* find_method(const ServiceDescriptorProto& service,
                                         std::string_view name) {
  for (const MethodDescriptorProto& method : service.method()) {
    if (method.name() == name) {
      return &method;
    }
  }
  return nullptr;
}

bool compare_file(const FileDescriptorProto& old_file, const FileDescriptorProto& current_file) {
  const std::string path = old_file.name();
  if (old_file.package() != current_file.package()) {
    return fail(path, "package changed");
  }

  for (const DescriptorProto& old_message : old_file.message_type()) {
    const DescriptorProto* current_message = find_message(current_file, old_message.name());
    if (current_message == nullptr) {
      return fail(path, "message was removed: " + old_message.name());
    }
    if (!compare_message(old_message, *current_message, path + "." + old_message.name())) {
      return false;
    }
  }
  for (const EnumDescriptorProto& old_enum : old_file.enum_type()) {
    const EnumDescriptorProto* current_enum = find_enum(current_file, old_enum.name());
    if (current_enum == nullptr) {
      return fail(path, "enum was removed: " + old_enum.name());
    }
    if (!compare_enum(old_enum, *current_enum, path + "." + old_enum.name())) {
      return false;
    }
  }
  for (const ServiceDescriptorProto& old_service : old_file.service()) {
    const ServiceDescriptorProto* current_service = find_service(current_file, old_service.name());
    if (current_service == nullptr) {
      return fail(path, "service was removed: " + old_service.name());
    }
    for (const MethodDescriptorProto& old_method : old_service.method()) {
      const MethodDescriptorProto* current_method =
          find_method(*current_service, old_method.name());
      if (current_method == nullptr) {
        return fail(path, "method was removed: " + old_method.name());
      }
      if (current_method->input_type() != old_method.input_type() ||
          current_method->output_type() != old_method.output_type() ||
          current_method->client_streaming() != old_method.client_streaming() ||
          current_method->server_streaming() != old_method.server_streaming()) {
        return fail(path, "method signature changed: " + old_method.name());
      }
    }
  }
  return true;
}

bool load_descriptor(const char* path, FileDescriptorSet* descriptor) {
  std::ifstream input(path, std::ios::binary);
  if (!input || !descriptor->ParseFromIstream(&input)) {
    return fail(path, "cannot parse descriptor set");
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: kiarabot_descriptor_compatibility CURRENT BASELINE\n";
    return 2;
  }
  FileDescriptorSet current;
  FileDescriptorSet baseline;
  if (!load_descriptor(argv[1], &current) || !load_descriptor(argv[2], &baseline)) {
    return 2;
  }

  for (const FileDescriptorProto& old_file : baseline.file()) {
    if (old_file.name().rfind("kiarabot/v1/", 0) != 0) {
      continue;
    }
    const FileDescriptorProto* current_file = find_file(current, old_file.name());
    if (current_file == nullptr) {
      fail(old_file.name(), "file was removed");
      return 1;
    }
    if (!compare_file(old_file, *current_file)) {
      return 1;
    }
  }
  std::cout << "Descriptor compatibility check passed\n";
  return 0;
}
