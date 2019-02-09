/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <map>
#include <string>
#include <queue>

#include "config.h"
#include "generator_helpers.h"
#include "node_generator.h"
#include "node_generator_helpers.h"
#include <google/protobuf/stubs/strutil.h>

using grpc::protobuf::Descriptor;
using grpc::protobuf::FileDescriptor;
using grpc::protobuf::MethodDescriptor;
using grpc::protobuf::ServiceDescriptor;
using grpc::protobuf::io::Printer;
using grpc::protobuf::io::StringOutputStream;
using std::map;

namespace grpc_node_generator {
namespace {

// Returns the alias we assign to the module of the given .proto filename
// when importing. Copied entirely from
// github:google/protobuf/src/google/protobuf/compiler/js/js_generator.cc#L154
grpc::string ModuleAlias(const grpc::string filename) {
  // This scheme could technically cause problems if a file includes any 2 of:
  //   foo/bar_baz.proto
  //   foo_bar_baz.proto
  //   foo_bar/baz.proto
  //
  // We'll worry about this problem if/when we actually see it.  This name isn't
  // exposed to users so we can change it later if we need to.
  grpc::string basename = grpc_generator::StripProto(filename);
  basename = grpc_generator::StringReplace(basename, "-", "$");
  basename = grpc_generator::StringReplace(basename, "/", "_");
  basename = grpc_generator::StringReplace(basename, ".", "_");
  return basename + "_pb";
}

// Given a filename like foo/bar/baz.proto, returns the corresponding JavaScript
// message file baz_pb.js
grpc::string GetJSMessageFilename(const grpc::string& filename) {
  std::vector<grpc::string> file_split = google::protobuf::Split(filename, "/");
  grpc::string name = file_split.back();
  return grpc_generator::StripProto(name) + "_pb.js";
}

// Given a filename like foo/bar/baz.proto, returns the relative directory to the import file
// or an npm module import
grpc::string GetRelativePath(const grpc::string& from_filename, const grpc::string& to_filename) {
  std::vector<grpc::string> source_file_split = google::protobuf::Split(from_filename, "/");
  std::vector<grpc::string> import_file_split = google::protobuf::Split(to_filename, "/");

  // remove filenames
  source_file_split.pop_back();
  import_file_split.pop_back();

  size_t source_path_length = source_file_split.size();

  size_t equal_parts_counter = 0;
  bool continue_running = true;

  while(continue_running) {
    if(source_file_split.empty() || import_file_split.empty()) {
        continue_running = false;
    } else {
      grpc::string& current_path_part = source_file_split.front();
      grpc::string& current_path_part_import = import_file_split.front();

      // one of the vectors became empty
      if(current_path_part == current_path_part_import){
        equal_parts_counter++;
        // Delete the first entry so we can continue searching for similarities in the rest of the path
        source_file_split.erase(source_file_split.begin());
        import_file_split.erase(import_file_split.begin());
      // stop once a part of the path doesn't match between source and import
      } else {
        continue_running = false;
      }
    }
  }
  
  if(source_path_length == equal_parts_counter && import_file_split.size() != 0) {
    // import from subdirectory
    // e.g. source -> /foo/baz.proto
    //      import -> /foo/bar/baz2.proto
    // expected return -> ./bar/
    import_file_split.insert(import_file_split.begin(), ".");
    return google::protobuf::JoinStrings(import_file_split, "/") +  "/";
  } else if(source_path_length == equal_parts_counter) {
    // Same directory import
    return "./";
  } else if (equal_parts_counter == 0) {
    // completely different package. Expect this to be npm package
    // e.g. google/protobuf
    return google::protobuf::JoinStrings(import_file_split, "/") +  "/";
  } else if (source_file_split.size() > import_file_split.size()) {
    // Import from higher directory
    // e.g. source /foo/bar/baz/baz.proto
    //      import /foo/bar.proto
    // expected return -> ../../
    std::vector<grpc::string> mapped_vector;
    mapped_vector.resize(source_file_split.size());
    // transform remaining path length to ../ 
    std::transform(source_file_split.begin(), source_file_split.end(), mapped_vector.begin(), [](grpc::string d) -> grpc::string { return "../"; });
    return google::protobuf::JoinStrings(mapped_vector, "");
  } else {
    // import from different subdirectory
    // e.g. source /foo/bar/baz/hoge.proto
    //      import /foo/bar/hoge/baz.proto
    // expected return -> ../hoge/
    std::vector<grpc::string> mapped_vector;
    mapped_vector.resize(source_file_split.size());
    // transform remaining path length to ../ 
    std::transform(source_file_split.begin(), source_file_split.end(), mapped_vector.begin(), [](grpc::string d) -> grpc::string { return "../"; });

    return google::protobuf::JoinStrings(mapped_vector, "") + google::protobuf::JoinStrings(import_file_split, "/") + "/";
  }
}

// Return the relative path to load to_file from the directory containing
// from_file, assuming that both paths are relative to the same directory
grpc::string GetRelativeImportPath(const grpc::string& from_file,
                             const grpc::string& to_file) {
  return GetRelativePath(from_file, to_file) + to_file;
}

/* Finds all message types used in all services in the file, and returns them
 * as a map of fully qualified message type name to message descriptor */
map<grpc::string, const Descriptor*> GetAllMessages(
    const FileDescriptor* file) {
  map<grpc::string, const Descriptor*> message_types;
  for (int service_num = 0; service_num < file->service_count();
       service_num++) {
    const ServiceDescriptor* service = file->service(service_num);
    for (int method_num = 0; method_num < service->method_count();
         method_num++) {
      const MethodDescriptor* method = service->method(method_num);
      const Descriptor* input_type = method->input_type();
      const Descriptor* output_type = method->output_type();
      message_types[input_type->full_name()] = input_type;
      message_types[output_type->full_name()] = output_type;
    }
  }
  return message_types;
}

grpc::string MessageIdentifierName(const grpc::string& name) {
  return grpc_generator::StringReplace(name, ".", "_");
}

grpc::string NodeObjectPath(const Descriptor* descriptor) {
  grpc::string module_alias = ModuleAlias(descriptor->file()->name());
  grpc::string name = descriptor->full_name();
  grpc_generator::StripPrefix(&name, descriptor->file()->package() + ".");
  return module_alias + "." + name;
}

// Prints out the message serializer and deserializer functions
void PrintMessageTransformer(const Descriptor* descriptor, Printer* out,
                             const Parameters& params) {
  map<grpc::string, grpc::string> template_vars;
  grpc::string full_name = descriptor->full_name();
  template_vars["identifier_name"] = MessageIdentifierName(full_name);
  template_vars["name"] = full_name;
  template_vars["node_name"] = NodeObjectPath(descriptor);
  // Print the serializer
  out->Print(template_vars, "function serialize_$identifier_name$(arg) {\n");
  out->Indent();
  out->Print(template_vars, "if (!(arg instanceof $node_name$)) {\n");
  out->Indent();
  out->Print(template_vars,
             "throw new Error('Expected argument of type $name$');\n");
  out->Outdent();
  out->Print("}\n");
  out->Print("return Buffer.from(arg.serializeBinary());\n");
  out->Outdent();
  out->Print("}\n\n");

  // Print the deserializer
  out->Print(template_vars,
             "function deserialize_$identifier_name$(buffer_arg) {\n");
  out->Indent();
  out->Print(
      template_vars,
      "return $node_name$.deserializeBinary(new Uint8Array(buffer_arg));\n");
  out->Outdent();
  out->Print("}\n\n");
}

void PrintMethod(const MethodDescriptor* method, Printer* out) {
  const Descriptor* input_type = method->input_type();
  const Descriptor* output_type = method->output_type();
  map<grpc::string, grpc::string> vars;
  vars["service_name"] = method->service()->full_name();
  vars["name"] = method->name();
  vars["input_type"] = NodeObjectPath(input_type);
  vars["input_type_id"] = MessageIdentifierName(input_type->full_name());
  vars["output_type"] = NodeObjectPath(output_type);
  vars["output_type_id"] = MessageIdentifierName(output_type->full_name());
  vars["client_stream"] = method->client_streaming() ? "true" : "false";
  vars["server_stream"] = method->server_streaming() ? "true" : "false";
  out->Print("{\n");
  out->Indent();
  out->Print(vars, "path: '/$service_name$/$name$',\n");
  out->Print(vars, "requestStream: $client_stream$,\n");
  out->Print(vars, "responseStream: $server_stream$,\n");
  out->Print(vars, "requestType: $input_type$,\n");
  out->Print(vars, "responseType: $output_type$,\n");
  out->Print(vars, "requestSerialize: serialize_$input_type_id$,\n");
  out->Print(vars, "requestDeserialize: deserialize_$input_type_id$,\n");
  out->Print(vars, "responseSerialize: serialize_$output_type_id$,\n");
  out->Print(vars, "responseDeserialize: deserialize_$output_type_id$,\n");
  out->Outdent();
  out->Print("}");
}

// Prints out the service descriptor object
void PrintService(const ServiceDescriptor* service, Printer* out) {
  map<grpc::string, grpc::string> template_vars;
  out->Print(GetNodeComments(service, true).c_str());
  template_vars["name"] = service->name();
  out->Print(template_vars, "var $name$Service = exports.$name$Service = {\n");
  out->Indent();
  for (int i = 0; i < service->method_count(); i++) {
    grpc::string method_name =
        grpc_generator::LowercaseFirstLetter(service->method(i)->name());
    out->Print(GetNodeComments(service->method(i), true).c_str());
    out->Print("$method_name$: ", "method_name", method_name);
    PrintMethod(service->method(i), out);
    out->Print(",\n");
    out->Print(GetNodeComments(service->method(i), false).c_str());
  }
  out->Outdent();
  out->Print("};\n\n");
  out->Print(template_vars,
             "exports.$name$Client = "
             "grpc.makeGenericClientConstructor($name$Service);\n");
  out->Print(GetNodeComments(service, false).c_str());
}

void PrintImports(const FileDescriptor* file, Printer* out) {
  out->Print("var grpc = require('grpc');\n");
  if (file->message_type_count() > 0) {
    grpc::string file_path =
        GetRelativeImportPath(file->name(), GetJSMessageFilename(file->name()));
    out->Print("var $module_alias$ = require('$file_path$');\n", "module_alias",
               ModuleAlias(file->name()), "file_path", file_path);
  }

  for (int i = 0; i < file->dependency_count(); i++) {
    grpc::string file_path = GetRelativeImportPath(
        file->name(), GetJSMessageFilename(file->dependency(i)->name()));
    out->Print("var $module_alias$ = require('$file_path$');\n", "module_alias",
               ModuleAlias(file->dependency(i)->name()), "file_path",
               file_path);
  }
  out->Print("\n");
}

void PrintTransformers(const FileDescriptor* file, Printer* out,
                       const Parameters& params) {
  map<grpc::string, const Descriptor*> messages = GetAllMessages(file);
  for (std::map<grpc::string, const Descriptor*>::iterator it =
           messages.begin();
       it != messages.end(); it++) {
    PrintMessageTransformer(it->second, out, params);
  }
  out->Print("\n");
}

void PrintServices(const FileDescriptor* file, Printer* out) {
  for (int i = 0; i < file->service_count(); i++) {
    PrintService(file->service(i), out);
  }
}
}  // namespace

grpc::string GenerateFile(const FileDescriptor* file,
                          const Parameters& params) {
  grpc::string output;
  {
    StringOutputStream output_stream(&output);
    Printer out(&output_stream, '$');

    if (file->service_count() == 0) {
      output = "// GENERATED CODE -- NO SERVICES IN PROTO";
      return output;
    }
    out.Print("// GENERATED CODE -- DO NOT EDIT!\n\n");

    grpc::string leading_comments = GetNodeComments(file, true);
    if (!leading_comments.empty()) {
      out.Print("// Original file comments:\n");
      out.PrintRaw(leading_comments.c_str());
    }

    out.Print("'use strict';\n");

    PrintImports(file, &out);

    PrintTransformers(file, &out, params);

    PrintServices(file, &out);

    out.Print(GetNodeComments(file, false).c_str());
  }
  return output;
}

}  // namespace grpc_node_generator
