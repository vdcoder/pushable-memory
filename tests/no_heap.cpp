#include <pushable_memory/flexible_buffer.hpp>
#include <cstdlib>
#include <new>
#include <cstdio>
static std::size_t calls=0;
void* operator new(std::size_t n) { ++calls; if(auto* p=std::malloc(n?n:1))return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
int main(){
    pushable_memory::FlexibleBuffer<> b(128);
    const auto before=calls;
    auto a=b.separate(b.end(),16); b.write(a,"abcdefghijklmnop",16);
    auto p=b.forward(a,4); auto q=b.separate(p,4); b.write(q,"1234",4);
    auto* ptr=b.c_ptr(p,8); auto* ptr2=b.c_ptr(b.forward(p,2),4); ptr2[0]='X';
    b.copy(a,p,4); b.c_ptr_release(ptr); b.c_ptr_release(ptr2);
    b.sink(q,4); auto* all=b.c_ptr(a,16); all[15]='!'; b.c_ptr_release(all);
    if(calls!=before || b.get(b.last())!='!')return 1;
    std::puts("No operator new calls during successful buffer operations.");
}
