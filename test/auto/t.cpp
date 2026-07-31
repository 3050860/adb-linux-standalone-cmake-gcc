#include <cassert>
#include <cstdio>
#include "libadb/libadb.h"
int main() {
    printf("version=%s number=%06x header=%s\n", libadb::version(),
           libadb::version_number(), LIBADB_VERSION_STRING);
    assert((libadb::version_number() >> 16) == LIBADB_VERSION_MAJOR);
    auto a = libadb::DeviceAddress::parse("192.168.1.10");
    auto b = libadb::DeviceAddress::parse("192.168.1.10:5037");
    auto c = libadb::DeviceAddress::parse("[::1]:5555");
    assert(a && b && c);
    printf("a=%s b=%s c=%s\n", a->to_string().c_str(), b->to_string().c_str(), c->to_string().c_str());
    assert(!libadb::DeviceAddress::parse("host:0"));
    assert(!libadb::DeviceAddress::parse("host:99999"));
    assert(!libadb::DeviceAddress::parse("host:abc"));
    assert(!libadb::DeviceAddress::parse(""));
    printf("status=%s cmd=%s phase=%s\n", libadb::to_string(libadb::Status::SlotTimeout),
           libadb::to_string(libadb::Command::Push), libadb::to_string(libadb::Phase::Commit));
    return 0;
}
