// Check the FileDescriptorProto actually advertised by the server, not just generated headers.
// Build: clang++ -std=c++17 tools/club_badge_schema_test.cpp $(pkg-config --cflags --libs protobuf) -o out/club_badge_schema_test.exe
// Usage: out/club_badge_schema_test.exe descriptor.bin
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <fstream>
#include <iostream>
#include <memory>

namespace gp = google::protobuf;
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: club_badge_schema_test descriptor.bin\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    gp::FileDescriptorProto file;
    if (!input || !file.ParseFromIstream(&input)) {
        std::cerr << "FAIL: cannot read FileDescriptorProto\n";
        return 1;
    }
    gp::DescriptorPool pool;
    if (!pool.BuildFile(file)) {
        std::cerr << "FAIL: cannot build advertised descriptor\n";
        return 1;
    }
    const auto* message = pool.FindMessageTypeByName("CreateBobRequest");
    const auto* field = message ? message->FindFieldByName("objectType") : nullptr;
    if (!field || field->number() != 5 || field->is_repeated() ||
        field->cpp_type() != gp::FieldDescriptor::CPPTYPE_ENUM) {
        std::cerr << "FAIL: CreateBobRequest.objectType must be singular enum field 5\n";
        return 1;
    }
    gp::DynamicMessageFactory factory(&pool);
    for (int number : {1, 5}) {
        const auto* value = field->enum_type()->FindValueByNumber(number);
        if (!value) {
            std::cerr << "FAIL: " << field->enum_type()->full_name()
                      << " lacks value " << number
                      << "; game 1.28 would pass NULL to Reflection::SetEnum\n";
            return 1;
        }
        std::unique_ptr<gp::Message> request(factory.GetPrototype(message)->New());
        request->GetReflection()->SetEnum(request.get(), field, value);
        const std::string wire = request->SerializeAsString();
        const std::string expected{char(0x28), char(number)};
        if (wire != expected || !request->ParseFromString(wire) ||
            request->GetReflection()->GetEnumValue(*request, field) != number) {
            std::cerr << "FAIL: enum wire value changed\n";
            return 1;
        }
        std::cout << "PASS: " << value->full_name() << "=" << number
                  << " resolves and round-trips as field 5\n";
    }
}
