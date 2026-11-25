#include <pushable_memory/flexible_buffer.hpp>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
using Buffer=pushable_memory::FlexibleBuffer<>;
// A minimal, self-delimiting binary document grammar for this demo:
// 01 object-start, 02 array-start, 03 end, 04 string (u8 length, bytes),
// 05 unsigned 32-bit integer (little endian). Object keys are strings.
// This is an illustration, not the final FlexDocument API or wire format.
Buffer::Address append_byte(Buffer& b, std::uint8_t v) {
    auto a=b.separate(b.end(),1); b.set(a,v); return a;
}
Buffer::Address insert_string(Buffer& b, Buffer::Address before, std::string_view s) {
    if(s.size()>255) throw std::length_error("demo string too long");
    auto a=b.separate(before,s.size()+2); b.set(a,4);
    b.set(b.forward(a,1),static_cast<std::uint8_t>(s.size()));
    b.write(b.forward(a,2),s.data(),s.size()); return a;
}
void append_string(Buffer& b,std::string_view s){(void)insert_string(b,b.end(),s);}
Buffer::Address append_u32(Buffer& b,std::uint32_t v) {
    auto a=b.separate(b.end(),5); b.set(a,5);
    for(std::size_t i=0;i<4;++i)b.set(b.forward(a,i+1),static_cast<std::uint8_t>(v>>(8*i)));
    return a;
}
void expect(bool b){if(!b)throw std::runtime_error("demo verification failed");}
int main(){
    Buffer b(512);
    append_byte(b,1); append_string(b,"name"); append_string(b,"Atlantis");
    append_string(b,"skills"); append_byte(b,2); append_string(b,"engineering");
    auto array_end=append_byte(b,3);
    append_string(b,"population"); auto population=append_u32(b,1000); append_byte(b,3);
    // Insert inside a nested array; the later population handle survives.
    auto physics=insert_string(b,array_end,"physics");
    expect(b.get(physics)==4 && b.get(population)==5);
    expect(b.get(b.forward(population,1))==0xe8);
    // Serialize the exact live byte stream; no tree traversal or re-encoding.
    std::stringstream stream(std::ios::in|std::ios::out|std::ios::binary);
    auto* p=b.c_ptr(b.first(),b.size());
    stream.write(reinterpret_cast<const char*>(p),static_cast<std::streamsize>(b.size()));
    b.c_ptr_release(p); expect(bool(stream));
    Buffer restored(512); auto start=restored.separate(restored.end(),b.size());
    auto* dst=restored.c_ptr(start,restored.size());
    stream.seekg(0); stream.read(reinterpret_cast<char*>(dst),static_cast<std::streamsize>(restored.size()));
    restored.c_ptr_release(dst); expect(bool(stream));
    for(std::size_t i=0;i<b.size();++i)expect(b.get(b.forward(b.first(),i))==restored.get(restored.forward(start,i)));
    std::cout<<"Nested document round-tripped "<<b.size()<<" bytes; surviving population address retained.\n";
}
