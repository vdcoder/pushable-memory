#include <pushable_memory/flexible_buffer.hpp>
#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <type_traits>
#include <vector>
using namespace pushable_memory;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while(false)
template<class E, class F> void throws(F f) { bool ok=false; try { f(); } catch(const E&) { ok=true; } CHECK(ok); }
using B=FlexibleBuffer<>;
static_assert(std::is_trivially_copyable_v<B::Address>);

template<bool Checked> void basics() {
    FlexibleBuffer<Checked> b(32);
    CHECK(b.first()==b.end() && b.last()==b.end());
    CHECK(b.separate(b.end(),0)==b.end());
    CHECK(b.c_ptr(b.end(),0)==nullptr); b.c_ptr_release(nullptr);
    b.read(b.end(),nullptr,0); b.write(b.end(),nullptr,0); b.sink(b.end(),0);
    auto a=b.separate(b.end(),6); b.write(a,"abcdef",6);
    auto c=b.forward(a,2), e=b.forward(a,4);
    auto q=b.separate(c,2); b.write(q,"XY",2);
    CHECK(b.get(c)=='c' && b.get(e)=='e');
    CHECK(b.forward(a,4)==c && b.backward(c,2)==q);
    CHECK(b.backward(b.end(),1)==b.last());
    CHECK(b.forward(b.last(),1)==b.end());
    b.sink(q,2); CHECK(b.forward(a,2)==c);
    b.copy(c,a,4); std::array<char,6> out{}; b.read(a,out.data(),6);
    CHECK((out==std::array<char,6>{'a','b','a','b','c','d'}));
    b.copy(a,c,4); b.read(a,out.data(),6);
    CHECK((out==std::array<char,6>{'a','b','c','d','c','d'}));
    b.sink(a,6); CHECK(b.size()==0);
    auto fresh=b.separate(b.end(),6); CHECK(fresh!=a && fresh!=q);
    throws<std::bad_alloc>([&]{(void)b.separate(b.end(),27);}); CHECK(b.size()==6);
}
void errors() {
    B b(8, {2, 10}); B other(4);
    auto a=b.separate(b.end(),5); b.write(a,"12345",5);
    throws<InvalidAddress>([&]{(void)b.get(other.end());});
    throws<InvalidAddress>([&]{(void)b.get(B::Address{});});
    throws<OutOfBounds>([&]{(void)b.get(b.end());});
    throws<OutOfBounds>([&]{(void)b.forward(b.end(),1);});
    throws<OutOfBounds>([&]{(void)b.backward(a,1);});
    throws<OutOfBounds>([&]{b.copy(a,b.last(),2);}); CHECK(b.get(a)=='1');
    throws<OutOfBounds>([&]{b.sink(a,6);}); CHECK(b.size()==5);
    auto dead=b.forward(a,2); b.sink(dead,1);
    auto fresh=b.separate(b.end(),1); CHECK(fresh!=dead);
    throws<InvalidAddress>([&]{(void)b.get(dead);});
    b.sink(b.first(),b.size());
    auto final=b.separate(b.end(),4); CHECK(final!=a);
    throws<AddressSpaceExhausted>([&]{(void)b.separate(b.end(),1);}); CHECK(b.size()==4);
    auto* p=b.c_ptr(final,1); auto* q=b.c_ptr(final,2); CHECK(p==q);
    throws<std::bad_alloc>([&]{(void)b.c_ptr(final,1);});
    b.c_ptr_release(q); b.sink(b.forward(final,1),1); b.c_ptr_release(p);
    throws<InvalidPointer>([&]{b.c_ptr_release(p);});
}
void pinning() {
    B b(24);
    auto a=b.separate(b.end(),8); b.write(a,"abcdefgh",8);
    auto c=b.forward(a,2), e=b.forward(a,4), g=b.forward(a,6);
    auto* p=b.c_ptr(c,4); auto* q=b.c_ptr(e,2); CHECK(q==p+2);
    q[0]='E'; CHECK(b.get(e)=='E' && p[2]=='E');
    b.set(c,'C'); CHECK(p[0]=='C');
    auto* extend=b.c_ptr(c,6); CHECK(extend==p); // compatible extension
    auto* other=b.c_ptr(a,2); other[1]='B'; CHECK(b.get(b.forward(a,1))=='B');
    throws<PinnedRangeConflict>([&]{(void)b.separate(e,1);});
    throws<PinnedRangeConflict>([&]{b.sink(g,1);});
    auto inserted=b.separate(c,2); b.write(inserted,"XY",2);
    CHECK(p[0]=='C' && extend[4]=='g' && other[1]=='B');
    // Logical ranges that used to be adjacent are now separated by XY.
    throws<PinnedRangeConflict>([&]{(void)b.c_ptr(a,6);});
    b.copy(c,a,2); CHECK(p[0]=='a' && p[1]=='B');
    b.c_ptr_release(other); b.c_ptr_release(q); b.c_ptr_release(extend);
    b.sink(g,2); // no longer covered by the shorter lease
    b.c_ptr_release(p); b.sink(inserted,2);
    auto* all=b.c_ptr(b.first(),b.size()); CHECK(all[0]=='a' && all[4]=='E');
    all[5]='!'; b.c_ptr_release(all); CHECK(b.get(b.last())=='!');
    // An insertion before a pinned range must not move its physical bytes.
    auto* last=b.c_ptr(b.last(),1); auto z=b.separate(b.first(),1); b.set(z,99);
    CHECK(*last=='!'); b.c_ptr_release(last);
}

template<bool Checked> void random_model(unsigned seed) {
    using M=FlexibleBuffer<Checked>; using A=typename M::Address;
    struct Cell { A addr; std::uint8_t value; };
    M b(128); std::vector<Cell> model; std::vector<A> dead;
    std::mt19937 r(seed);
    auto at=[&](std::size_t i){return i==model.size()?b.end():model[i].addr;};
    for(int step=0;step<12000;++step) {
        const auto n=model.size(); const auto op=r()%7;
        if ((op==0 || n==0) && n<128) {
            auto pos=r()%(n+1), count=std::min<std::size_t>(1+r()%12,128-n);
            auto start=b.separate(at(pos),count); std::vector<Cell> new_cells;
            for(std::size_t i=0;i<count;++i) {
                auto a=b.forward(start,i); auto value=static_cast<std::uint8_t>(r()); b.set(a,value);
                for(auto old:dead) CHECK(old!=a);
                new_cells.push_back({a,value});
            }
            model.insert(model.begin()+pos,new_cells.begin(),new_cells.end());
        } else if(n && op==1) {
            auto pos=r()%n, count=1+r()%(n-pos); b.sink(at(pos),count);
            for(std::size_t i=0;i<count;++i) dead.push_back(model[pos+i].addr);
            model.erase(model.begin()+pos,model.begin()+pos+count);
        } else if(n && op==2) {
            auto s=r()%n,d=r()%n,k=r()%(std::min(n-s,n-d)+1); std::vector<std::uint8_t> tmp;
            for(std::size_t i=0;i<k;++i) tmp.push_back(model[s+i].value);
            b.copy(at(d),at(s),k); for(std::size_t i=0;i<k;++i) model[d+i].value=tmp[i];
        } else if(n && op==3) {
            auto pos=r()%n,k=1+r()%(n-pos); auto* p=b.c_ptr(at(pos),k);
            for(std::size_t i=0;i<k;++i) { CHECK(p[i]==model[pos+i].value); p[i]=static_cast<std::uint8_t>(r()); model[pos+i].value=p[i]; }
            if(k>1) { auto* q=b.c_ptr(at(pos+1),k-1); CHECK(q==p+1); q[0]^=42; model[pos+1].value=q[0]; b.c_ptr_release(p); CHECK(q[0]==model[pos+1].value); b.c_ptr_release(q); }
            else b.c_ptr_release(p);
        } else if(n && op==4) {
            auto pos=r()%n; auto v=static_cast<std::uint8_t>(r()); b.set(at(pos),v); model[pos].value=v;
        } else if(n && op==5) {
            auto pos=r()%n,k=1+r()%(n-pos); std::vector<std::uint8_t> tmp(k); for(auto& v:tmp)v=static_cast<std::uint8_t>(r());
            b.write(at(pos),tmp.data(),k); for(std::size_t i=0;i<k;++i)model[pos+i].value=tmp[i];
        }
        CHECK(b.size()==model.size()); CHECK(b.first()==at(0));
        CHECK(b.last()==(model.empty()?b.end():at(model.size()-1)));
        std::vector<std::uint8_t> out(model.size()); b.read(b.first(),out.data(),out.size());
        for(std::size_t i=0;i<model.size();++i) {
            CHECK(b.forward(b.first(),i)==at(i)); CHECK(b.backward(b.end(),model.size()-i)==at(i));
            CHECK(b.get(at(i))==model[i].value && out[i]==model[i].value);
        }
        if constexpr(Checked) if(!dead.empty()) throws<InvalidAddress>([&]{(void)b.get(dead[r()%dead.size()]);});
    }
}
int main() {
    basics<true>(); basics<false>(); errors(); pinning();
    for(unsigned seed : {1u,7u,42u,123456u}) { random_model<true>(seed); random_model<false>(seed); }
    std::cout<<"All reference tests passed (96,000 randomized operations).\n";
}
