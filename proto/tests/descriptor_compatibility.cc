#include <algorithm>
#include <fstream>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/message_differencer.h>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using google::protobuf::DescriptorProto;
using google::protobuf::EnumDescriptorProto;
using google::protobuf::EnumValueDescriptorProto;
using google::protobuf::FieldDescriptorProto;
using google::protobuf::FileDescriptorProto;
using google::protobuf::FileDescriptorSet;
using google::protobuf::MethodDescriptorProto;
using google::protobuf::OneofDescriptorProto;
using google::protobuf::ServiceDescriptorProto;

bool fail(std::string_view path, std::string_view message) {
  std::cerr << path << ": " << message << '\n';
  return false;
}

template <typename Message>
bool same_options(const Message& old_message, const Message& current_message) {
  return google::protobuf::util::MessageDifferencer::Equals(old_message.options(),
                                                            current_message.options());
}

template <typename Range>
bool contains_range(const google::protobuf::RepeatedPtrField<Range>& current_ranges,
                    const Range& old_range) {
  return std::any_of(
      current_ranges.begin(), current_ranges.end(), [&old_range](const Range& current_range) {
        return current_range.start() == old_range.start() && current_range.end() == old_range.end();
      });
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

const FieldDescriptorProto* find_extension_by_number(const DescriptorProto& message, int number) {
  for (const FieldDescriptorProto& field : message.extension()) {
    if (field.number() == number) {
      return &field;
    }
  }
  return nullptr;
}

const OneofDescriptorProto* find_oneof(const DescriptorProto& message, std::string_view name) {
  for (const OneofDescriptorProto& oneof : message.oneof_decl()) {
    if (oneof.name() == name) {
      return &oneof;
    }
  }
  return nullptr;
}

std::string oneof_name(const DescriptorProto& message, const FieldDescriptorProto& field) {
  if (!field.has_oneof_index() || field.oneof_index() < 0 ||
      field.oneof_index() >= message.oneof_decl_size()) {
    return {};
  }
  return message.oneof_decl(field.oneof_index()).name();
}

bool compare_field(const FieldDescriptorProto& old_field, const FieldDescriptorProto& current_field,
                   const std::string& path, std::string_view old_oneof_name,
                   std::string_view current_oneof_name) {
  if (old_field.name() != current_field.name() || old_field.number() != current_field.number() ||
      old_field.label() != current_field.label() || old_field.type() != current_field.type() ||
      old_field.type_name() != current_field.type_name() ||
      old_field.extendee() != current_field.extendee() ||
      old_field.has_default_value() != current_field.has_default_value() ||
      old_field.default_value() != current_field.default_value() ||
      old_field.json_name() != current_field.json_name() ||
      old_field.proto3_optional() != current_field.proto3_optional() ||
      old_field.has_oneof_index() != current_field.has_oneof_index() ||
      old_oneof_name != current_oneof_name || !same_options(old_field, current_field)) {
    return fail(path, "field definition changed: " + old_field.name());
  }
  return true;
}

bool compare_enum_value(const EnumValueDescriptorProto& old_value,
                        const EnumDescriptorProto& current_enum, const std::string& path) {
  const auto current =
      std::find_if(current_enum.value().begin(), current_enum.value().end(),
                   [&old_value](const auto& value) { return value.name() == old_value.name(); });
  if (current == current_enum.value().end()) {
    return fail(path, "enum value was removed: " + old_value.name());
  }
  if (current->number() != old_value.number() || !same_options(old_value, *current)) {
    return fail(path, "enum value definition changed: " + old_value.name());
  }
  return true;
}

bool compare_enum(const EnumDescriptorProto& old_enum, const EnumDescriptorProto& current_enum,
                  const std::string& path) {
  if (!same_options(old_enum, current_enum)) {
    return fail(path, "enum options changed");
  }
  for (const EnumValueDescriptorProto& old_value : old_enum.value()) {
    if (!compare_enum_value(old_value, current_enum, path)) {
      return false;
    }
  }
  for (const auto& old_range : old_enum.reserved_range()) {
    if (!contains_range(current_enum.reserved_range(), old_range)) {
      return fail(path, "enum reserved range was removed");
    }
  }
  for (const std::string& old_name : old_enum.reserved_name()) {
    if (std::find(current_enum.reserved_name().begin(), current_enum.reserved_name().end(),
                  old_name) == current_enum.reserved_name().end()) {
      return fail(path, "enum reserved name was removed: " + old_name);
    }
  }
  return true;
}

bool compare_message(const DescriptorProto& old_message, const DescriptorProto& current_message,
                     const std::string& path) {
  if (!same_options(old_message, current_message)) {
    return fail(path, "message options changed");
  }

  for (const FieldDescriptorProto& old_field : old_message.field()) {
    const FieldDescriptorProto* current_field =
        find_field_by_number(current_message, old_field.number());
    if (current_field == nullptr) {
      return fail(path, "field number was removed: " + std::to_string(old_field.number()));
    }
    if (!compare_field(old_field, *current_field, path, oneof_name(old_message, old_field),
                       oneof_name(current_message, *current_field))) {
      return false;
    }
  }

  for (const FieldDescriptorProto& old_extension : old_message.extension()) {
    const FieldDescriptorProto* current_extension =
        find_extension_by_number(current_message, old_extension.number());
    if (current_extension == nullptr) {
      return fail(path, "extension number was removed: " + std::to_string(old_extension.number()));
    }
    if (!compare_field(old_extension, *current_extension, path, {}, {})) {
      return false;
    }
  }

  for (const OneofDescriptorProto& old_oneof : old_message.oneof_decl()) {
    const OneofDescriptorProto* current_oneof = find_oneof(current_message, old_oneof.name());
    if (current_oneof == nullptr || !same_options(old_oneof, *current_oneof)) {
      return fail(path, "oneof definition was removed or changed: " + old_oneof.name());
    }
  }

  for (const auto& old_range : old_message.extension_range()) {
    if (!contains_range(current_message.extension_range(), old_range)) {
      return fail(path, "extension range was removed");
    }
  }
  for (const auto& old_range : old_message.reserved_range()) {
    if (!contains_range(current_message.reserved_range(), old_range)) {
      return fail(path, "message reserved range was removed");
    }
  }
  for (const std::string& old_name : old_message.reserved_name()) {
    if (std::find(current_message.reserved_name().begin(), current_message.reserved_name().end(),
                  old_name) == current_message.reserved_name().end()) {
      return fail(path, "message reserved name was removed: " + old_name);
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
  if (old_file.syntax() != current_file.syntax() || old_file.package() != current_file.package()) {
    return fail(path, "syntax or package changed");
  }
  if (!same_options(old_file, current_file)) {
    return fail(path, "file options changed");
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
  for (const FieldDescriptorProto& old_extension : old_file.extension()) {
    const auto current = std::find_if(
        current_file.extension().begin(), current_file.extension().end(),
        [&old_extension](const auto& field) { return field.number() == old_extension.number(); });
    if (current == current_file.extension().end()) {
      return fail(path, "file extension was removed: " + old_extension.name());
    }
    if (!compare_field(old_extension, *current, path, {}, {})) {
      return false;
    }
  }
  for (const ServiceDescriptorProto& old_service : old_file.service()) {
    const ServiceDescriptorProto* current_service = find_service(current_file, old_service.name());
    if (current_service == nullptr) {
      return fail(path, "service was removed: " + old_service.name());
    }
    if (!same_options(old_service, *current_service)) {
      return fail(path, "service options changed: " + old_service.name());
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
          current_method->server_streaming() != old_method.server_streaming() ||
          !same_options(old_method, *current_method)) {
        return fail(path, "method signature or options changed: " + old_method.name());
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

  bool has_baseline_contract = false;
  for (const FileDescriptorProto& old_file : baseline.file()) {
    if (old_file.name().rfind("kiarabot/v1/", 0) != 0) {
      continue;
    }
    has_baseline_contract = true;
    const FileDescriptorProto* current_file = find_file(current, old_file.name());
    if (current_file == nullptr) {
      fail(old_file.name(), "file was removed");
      return 1;
    }
    if (!compare_file(old_file, *current_file)) {
      return 1;
    }
  }
  if (!has_baseline_contract) {
    fail(argv[2], "baseline contains no kiarabot/v1 files");
    return 2;
  }
  std::cout << "Descriptor compatibility check passed\n";
  return 0;
}
