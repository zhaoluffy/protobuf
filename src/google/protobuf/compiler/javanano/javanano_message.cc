// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// http://code.google.com/p/protobuf/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: kenton@google.com (Kenton Varda)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.

#include <algorithm>
#include <google/protobuf/stubs/hash.h>
#include <google/protobuf/compiler/javanano/javanano_message.h>
#include <google/protobuf/compiler/javanano/javanano_enum.h>
#include <google/protobuf/compiler/javanano/javanano_extension.h>
#include <google/protobuf/compiler/javanano/javanano_helpers.h>
#include <google/protobuf/stubs/strutil.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format.h>
#include <google/protobuf/descriptor.pb.h>

namespace google {
namespace protobuf {
namespace compiler {
namespace javanano {

using internal::WireFormat;
using internal::WireFormatLite;

namespace {

void PrintFieldComment(io::Printer* printer, const FieldDescriptor* field) {
  // Print the field's proto-syntax definition as a comment.  We don't want to
  // print group bodies so we cut off after the first line.
  string def = field->DebugString();
  printer->Print("// $def$\n",
    "def", def.substr(0, def.find_first_of('\n')));
}

struct FieldOrderingByNumber {
  inline bool operator()(const FieldDescriptor* a,
                         const FieldDescriptor* b) const {
    return a->number() < b->number();
  }
};

// Sort the fields of the given Descriptor by number into a new[]'d array
// and return it.
const FieldDescriptor** SortFieldsByNumber(const Descriptor* descriptor) {
  const FieldDescriptor** fields =
    new const FieldDescriptor*[descriptor->field_count()];
  for (int i = 0; i < descriptor->field_count(); i++) {
    fields[i] = descriptor->field(i);
  }
  sort(fields, fields + descriptor->field_count(),
       FieldOrderingByNumber());
  return fields;
}

// Get an identifier that uniquely identifies this type within the file.
// This is used to declare static variables related to this type at the
// outermost file scope.
string UniqueFileScopeIdentifier(const Descriptor* descriptor) {
  return "static_" + StringReplace(descriptor->full_name(), ".", "_", true);
}

}  // namespace

// ===================================================================

MessageGenerator::MessageGenerator(const Descriptor* descriptor, const Params& params)
  : params_(params),
    descriptor_(descriptor),
    field_generators_(descriptor, params) {
}

MessageGenerator::~MessageGenerator() {}

void MessageGenerator::GenerateStaticVariables(io::Printer* printer) {
  // Generate static members for all nested types.
  for (int i = 0; i < descriptor_->nested_type_count(); i++) {
    // TODO(kenton):  Reuse MessageGenerator objects?
    MessageGenerator(descriptor_->nested_type(i), params_)
      .GenerateStaticVariables(printer);
  }
}

void MessageGenerator::GenerateStaticVariableInitializers(
    io::Printer* printer) {
  // Generate static member initializers for all nested types.
  for (int i = 0; i < descriptor_->nested_type_count(); i++) {
   // TODO(kenton):  Reuse MessageGenerator objects?
    MessageGenerator(descriptor_->nested_type(i), params_)
      .GenerateStaticVariableInitializers(printer);
  }
}

void MessageGenerator::Generate(io::Printer* printer) {
  if (!params_.store_unknown_fields() &&
      (descriptor_->extension_count() != 0 || descriptor_->extension_range_count() != 0)) {
    GOOGLE_LOG(FATAL) << "Extensions are only supported in NANO_RUNTIME if the "
        "'store_unknown_fields' generator option is 'true'\n";
  }

  const string& file_name = descriptor_->file()->name();
  bool is_own_file =
    params_.java_multiple_files(file_name)
      && descriptor_->containing_type() == NULL;

  if (is_own_file) {
    // Note: constants (from enums and fields requiring stored defaults, emitted in the loop below)
    // may have the same names as constants in the nested classes. This causes Java warnings, but
    // is not fatal, so we suppress those warnings here in the top-most class declaration.
    printer->Print(
      "\n"
      "@SuppressWarnings(\"hiding\")\n"
      "public final class $classname$ extends\n",
      "classname", descriptor_->name());
  } else {
    printer->Print(
      "\n"
      "public static final class $classname$ extends\n",
      "classname", descriptor_->name());
  }
  if (params_.store_unknown_fields()) {
    printer->Print(
      "    com.google.protobuf.nano.ExtendableMessageNano {\n");
  } else {
    printer->Print(
      "    com.google.protobuf.nano.MessageNano {\n");
  }
  printer->Indent();
  printer->Print(
    "\n"
    "public static final $classname$[] EMPTY_ARRAY = {};\n"
    "\n"
    "public $classname$() {\n"
    "  clear();\n"
    "}\n",
    "classname", descriptor_->name());

  // Nested types and extensions
  for (int i = 0; i < descriptor_->extension_count(); i++) {
    ExtensionGenerator(descriptor_->extension(i), params_).Generate(printer);
  }

  for (int i = 0; i < descriptor_->enum_type_count(); i++) {
    EnumGenerator(descriptor_->enum_type(i), params_).Generate(printer);
  }

  for (int i = 0; i < descriptor_->nested_type_count(); i++) {
    MessageGenerator(descriptor_->nested_type(i), params_).Generate(printer);
  }

  // Integers for bit fields
  int totalInts = (field_generators_.total_bits() + 31) / 32;
  if (totalInts > 0) {
    printer->Print("\n");
    for (int i = 0; i < totalInts; i++) {
      printer->Print("private int $bit_field_name$;\n",
        "bit_field_name", GetBitFieldName(i));
    }
  }

  // Fields
  for (int i = 0; i < descriptor_->field_count(); i++) {
    printer->Print("\n");
    PrintFieldComment(printer, descriptor_->field(i));
    field_generators_.get(descriptor_->field(i)).GenerateMembers(printer);
  }

  GenerateClear(printer);

  if (params_.generate_equals()) {
    GenerateEquals(printer);
    GenerateHashCode(printer);
  }

  GenerateMessageSerializationMethods(printer);
  GenerateMergeFromMethods(printer);
  GenerateParseFromMethods(printer);

  printer->Outdent();
  printer->Print("}\n");
}

// ===================================================================

void MessageGenerator::
GenerateMessageSerializationMethods(io::Printer* printer) {
  scoped_array<const FieldDescriptor*> sorted_fields(
    SortFieldsByNumber(descriptor_));

  // writeTo only throws an exception if it contains one or more fields to write
  if (descriptor_->field_count() > 0 || params_.store_unknown_fields()) {
    printer->Print(
      "\n"
      "@Override\n"
      "public void writeTo(com.google.protobuf.nano.CodedOutputByteBufferNano output)\n"
      "    throws java.io.IOException {\n");
  } else {
    printer->Print(
      "\n"
      "@Override\n"
      "public void writeTo(com.google.protobuf.nano.CodedOutputByteBufferNano output) {\n");
  }
  printer->Indent();

  // Output the fields in sorted order
  for (int i = 0; i < descriptor_->field_count(); i++) {
    GenerateSerializeOneField(printer, sorted_fields[i]);
  }

  // Write unknown fields.
  if (params_.store_unknown_fields()) {
    printer->Print(
      "com.google.protobuf.nano.WireFormatNano.writeUnknownFields(\n"
      "    unknownFieldData, output);\n");
  }

  printer->Outdent();
  printer->Print("}\n");

  // Rely on the parent implementation of getSerializedSize if there are no fields to
  // serialize in this MessageNano.
  if (descriptor_->field_count() != 0) {
    printer->Print(
      "\n"
      "@Override\n"
      "public int getSerializedSize() {\n"
      "  int size = super.getSerializedSize();\n");
    printer->Indent();

    for (int i = 0; i < descriptor_->field_count(); i++) {
      field_generators_.get(sorted_fields[i]).GenerateSerializedSizeCode(printer);
    }

    printer->Outdent();
    printer->Print(
      "  cachedSize = size;\n"
      "  return size;\n"
      "}\n");
  }
}

void MessageGenerator::GenerateMergeFromMethods(io::Printer* printer) {
  scoped_array<const FieldDescriptor*> sorted_fields(
    SortFieldsByNumber(descriptor_));

  printer->Print(
    "\n"
    "@Override\n"
    "public $classname$ mergeFrom(\n"
    "        com.google.protobuf.nano.CodedInputByteBufferNano input)\n"
    "    throws java.io.IOException {\n",
    "classname", descriptor_->name());

  printer->Indent();

  printer->Print(
    "while (true) {\n");
  printer->Indent();

  printer->Print(
    "int tag = input.readTag();\n"
    "switch (tag) {\n");
  printer->Indent();

  printer->Print(
    "case 0:\n"          // zero signals EOF / limit reached
    "  return this;\n"
    "default: {\n");

  printer->Indent();
  if (params_.store_unknown_fields()) {
    printer->Print(
        "if (unknownFieldData == null) {\n"
        "  unknownFieldData =\n"
        "      new java.util.ArrayList<com.google.protobuf.nano.UnknownFieldData>();\n"
        "}\n"
        "if (!com.google.protobuf.nano.WireFormatNano.storeUnknownField(\n"
        "    unknownFieldData, input, tag)) {\n"
        "  return this;\n"
        "}\n");
  } else {
    printer->Print(
        "if (!com.google.protobuf.nano.WireFormatNano.parseUnknownField(input, tag)) {\n"
        "  return this;\n"   // it's an endgroup tag
        "}\n");
  }
  printer->Print("break;\n");
  printer->Outdent();
  printer->Print("}\n");

  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = sorted_fields[i];
    uint32 tag = WireFormatLite::MakeTag(field->number(),
      WireFormat::WireTypeForField(field));

    printer->Print(
      "case $tag$: {\n",
      "tag", SimpleItoa(tag));
    printer->Indent();

    field_generators_.get(field).GenerateMergingCode(printer);

    printer->Outdent();
    printer->Print(
      "  break;\n"
      "}\n");
  }

  printer->Outdent();
  printer->Outdent();
  printer->Outdent();
  printer->Print(
    "    }\n"     // switch (tag)
    "  }\n"       // while (true)
    "}\n");
}

void MessageGenerator::
GenerateParseFromMethods(io::Printer* printer) {
  // Note:  These are separate from GenerateMessageSerializationMethods()
  //   because they need to be generated even for messages that are optimized
  //   for code size.
  printer->Print(
    "\n"
    "public static $classname$ parseFrom(byte[] data)\n"
    "    throws com.google.protobuf.nano.InvalidProtocolBufferNanoException {\n"
    "  return com.google.protobuf.nano.MessageNano.mergeFrom(new $classname$(), data);\n"
    "}\n"
    "\n"
    "public static $classname$ parseFrom(\n"
    "        com.google.protobuf.nano.CodedInputByteBufferNano input)\n"
    "    throws java.io.IOException {\n"
    "  return new $classname$().mergeFrom(input);\n"
    "}\n",
    "classname", descriptor_->name());
}

void MessageGenerator::GenerateSerializeOneField(
    io::Printer* printer, const FieldDescriptor* field) {
  field_generators_.get(field).GenerateSerializationCode(printer);
}

void MessageGenerator::GenerateClear(io::Printer* printer) {
  printer->Print(
    "\n"
    "public $classname$ clear() {\n",
    "classname", descriptor_->name());
  printer->Indent();

  // Clear bit fields.
  int totalInts = (field_generators_.total_bits() + 31) / 32;
  for (int i = 0; i < totalInts; i++) {
    printer->Print("$bit_field_name$ = 0;\n",
      "bit_field_name", GetBitFieldName(i));
  }

  // Call clear for all of the fields.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    field_generators_.get(field).GenerateClearCode(printer);
  }

  // Clear unknown fields.
  if (params_.store_unknown_fields()) {
    printer->Print("unknownFieldData = null;\n");
  }

  printer->Outdent();
  printer->Print(
    "  cachedSize = -1;\n"
    "  return this;\n"
    "}\n");
}

void MessageGenerator::GenerateEquals(io::Printer* printer) {
  // Don't override if there are no fields. We could generate an
  // equals method that compares types, but often empty messages
  // are used as namespaces.
  if (descriptor_->field_count() == 0 && !params_.store_unknown_fields()) {
    return;
  }

  printer->Print(
    "\n"
    "@Override\n"
    "public boolean equals(Object o) {\n");
  printer->Indent();
  printer->Print(
    "if (o == this) {\n"
    "  return true;\n"
    "}\n"
    "if (!(o instanceof $classname$)) {\n"
    "  return false;\n"
    "}\n"
    "$classname$ other = ($classname$) o;\n",
    "classname", descriptor_->name());

  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    field_generators_.get(field).GenerateEqualsCode(printer);
  }

  if (params_.store_unknown_fields()) {
    printer->Print(
      "if (unknownFieldData == null || unknownFieldData.isEmpty()) {\n"
      "  return other.unknownFieldData == null || other.unknownFieldData.isEmpty();"
      "} else {\n"
      "  return unknownFieldData.equals(other.unknownFieldData);\n"
      "}\n");
  } else {
    printer->Print(
      "return true;\n");
  }

  printer->Outdent();
  printer->Print("}\n");
}

void MessageGenerator::GenerateHashCode(io::Printer* printer) {
  if (descriptor_->field_count() == 0 && !params_.store_unknown_fields()) {
    return;
  }

  printer->Print(
    "\n"
    "@Override\n"
    "public int hashCode() {\n");
  printer->Indent();

  printer->Print("int result = 17;\n");
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    field_generators_.get(field).GenerateHashCodeCode(printer);
  }

  if (params_.store_unknown_fields()) {
    printer->Print(
      "result = 31 * result + (unknownFieldData == null || unknownFieldData.isEmpty()\n"
      "    ? 0 : unknownFieldData.hashCode());\n");
  }

  printer->Print("return result;\n");

  printer->Outdent();
  printer->Print("}\n");
}

// ===================================================================

}  // namespace javanano
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
