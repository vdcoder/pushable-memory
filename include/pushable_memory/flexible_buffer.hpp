#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <vector>

namespace pushable_memory {

struct InvalidAddress : std::exception {
    const char* what() const noexcept override { return "invalid flexible-memory address"; }
};
struct OutOfBounds : std::exception {
    const char* what() const noexcept override { return "flexible-memory range out of bounds"; }
};
struct AddressSpaceExhausted : std::exception {
    const char* what() const noexcept override { return "flexible-memory address space exhausted"; }
};
struct PinnedRangeConflict : std::exception {
    const char* what() const noexcept override { return "operation conflicts with a native pointer lease"; }
};
struct InvalidPointer : std::exception {
    const char* what() const noexcept override { return "invalid native pointer release"; }
};

// Reference-only resource knobs. The second one permits testing exhaustion
// without issuing 2^64 addresses. Neither changes normal address semantics.
struct ReferenceOptions {
    std::size_t max_leases = 0; // 0 => max(capacity, 16)
    std::uint64_t address_limit = std::numeric_limits<std::uint64_t>::max();
};

namespace detail {
inline std::atomic<std::uint64_t> next_owner{1};
inline std::uint64_t new_owner() {
    auto id = next_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (id == 0) throw AddressSpaceExhausted{};
        const auto next = id == std::numeric_limits<std::uint64_t>::max() ? 0 : id + 1;
        if (next_owner.compare_exchange_weak(id, next, std::memory_order_relaxed)) return id;
    }
}
} // namespace detail

template<bool BoundsChecked = true>
class FlexibleBuffer {
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
public:
    class Address {
        friend class FlexibleBuffer;
        std::uint64_t owner_ = 0;
        std::uint64_t generation_ = 0;
        std::size_t slot_ = none;
        constexpr Address(std::uint64_t owner, std::uint64_t gen, std::size_t slot) noexcept
            : owner_(owner), generation_(gen), slot_(slot) {}
    public:
        constexpr Address() noexcept = default;
        friend constexpr bool operator==(Address, Address) noexcept = default;
    };

    explicit FlexibleBuffer(std::size_t capacity, ReferenceOptions options = {})
        : owner_(detail::new_owner()), address_limit_(options.address_limit),
          max_leases_(options.max_leases ? options.max_leases : std::max(capacity, std::size_t{16})),
          bytes_(capacity), slots_(capacity), physical_to_slot_(capacity, none),
          pins_(capacity, 0) {
        order_.reserve(capacity);
        free_slots_.reserve(capacity);
        leases_.reserve(max_leases_);
        for (std::size_t i = capacity; i != 0; --i) free_slots_.push_back(i - 1);
    }
    FlexibleBuffer(const FlexibleBuffer&) = delete;
    FlexibleBuffer& operator=(const FlexibleBuffer&) = delete;
    FlexibleBuffer(FlexibleBuffer&&) = delete;
    FlexibleBuffer& operator=(FlexibleBuffer&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return bytes_.size(); }
    [[nodiscard]] Address end() const noexcept { return Address(owner_, 0, none); }
    [[nodiscard]] Address first() const noexcept { return address_at(0); }
    [[nodiscard]] Address last() const noexcept { return size() ? address_at(size()-1) : end(); }

    [[nodiscard]] Address separate(Address start, std::size_t count) {
        const auto pos = position(start);
        if (!count) return start;
        if (count > capacity() - size()) throw std::bad_alloc{};
        if (count > address_limit_ - issued_) throw AddressSpaceExhausted{};
        // Inserting between two cells of any one lease breaks its contiguity.
        for (const auto& lease : leases_) {
            const auto begin = slots_[physical_to_slot_[lease.physical]].position;
            if (pos > begin && pos - begin < lease.count) throw PinnedRangeConflict{};
        }
        // Every possible failure has been checked. All vectors have reserved capacity.
        const auto old_size = size();
        order_.resize(old_size + count);
        std::move_backward(order_.begin() + pos, order_.begin() + old_size, order_.end());
        std::size_t physical = 0;
        for (std::size_t i = 0; i < count; ++i) {
            while (physical_to_slot_[physical] != none) ++physical;
            const auto slot = free_slots_.back();
            free_slots_.pop_back();
            slots_[slot] = Slot{pos + i, physical, ++issued_};
            order_[pos + i] = slot;
            physical_to_slot_[physical++] = slot;
            // Deliberately do not initialize the new cell's byte.
        }
        refresh_positions(pos + count);
        return address_at(pos);
    }

    void sink(Address start, std::size_t count) {
        const auto pos = range(start, count);
        if (!count) return;
        for (std::size_t i = 0; i < count; ++i)
            if (pins_[slots_[order_[pos+i]].physical]) throw PinnedRangeConflict{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto slot = order_[pos+i];
            physical_to_slot_[slots_[slot].physical] = none;
            slots_[slot].position = none;
            slots_[slot].physical = none;
            free_slots_.push_back(slot);
        }
        order_.erase(order_.begin()+pos, order_.begin()+pos+count);
        refresh_positions(pos);
    }

    [[nodiscard]] Address forward(Address from, std::size_t count) const {
        const auto p = range(from, count);
        return address_at(p + count);
    }
    [[nodiscard]] Address backward(Address from, std::size_t count) const {
        const auto p = position(from);
        if constexpr (BoundsChecked) { if (count > p) throw OutOfBounds{}; }
        return address_at(p-count);
    }
    [[nodiscard]] std::uint8_t get(Address address) const { return byte_at(range(address,1)); }
    void set(Address address, std::uint8_t value) { byte_at(range(address,1)) = value; }
    void read(Address start, void* dst, std::size_t count) const {
        const auto p = range(start,count);
        auto* out = static_cast<std::uint8_t*>(dst);
        for (std::size_t i=0; i<count; ++i) out[i] = byte_at(p+i);
    }
    void write(Address start, const void* src, std::size_t count) {
        const auto p = range(start,count);
        const auto* in = static_cast<const std::uint8_t*>(src);
        for (std::size_t i=0; i<count; ++i) byte_at(p+i) = in[i];
    }
    void copy(Address dst, Address src, std::size_t count) {
        const auto d = range(dst,count), s = range(src,count);
        if (d > s && d-s < count) {
            for (auto i=count; i!=0; --i) byte_at(d+i-1) = byte_at(s+i-1);
        } else {
            for (std::size_t i=0; i<count; ++i) byte_at(d+i) = byte_at(s+i);
        }
    }

    // Make the requested cells physically contiguous, without moving ANY pinned
    // cell. If a requested cell is already pinned, its position fixes the only
    // possible starting physical address for the entire range.
    [[nodiscard]] std::uint8_t* c_ptr(Address start, std::size_t count) {
        const auto p = range(start,count);
        if (!count) return nullptr;
        if (leases_.size() == max_leases_) throw std::bad_alloc{};
        std::size_t target = none;
        bool anchored = false;
        for (std::size_t i=0; i<count; ++i) {
            const auto phys = slots_[order_[p+i]].physical;
            if (!pins_[phys]) continue;
            if (phys < i || phys-i > capacity()-count) throw PinnedRangeConflict{};
            const auto candidate = phys-i;
            if (anchored && target != candidate) throw PinnedRangeConflict{};
            target = candidate;
            anchored = true;
        }
        if (!anchored) {
            // Prefer the existing location if it is already contiguous.
            const auto existing = slots_[order_[p]].physical;
            bool contiguous = existing <= capacity()-count;
            for (std::size_t i=0; contiguous && i<count; ++i)
                contiguous = slots_[order_[p+i]].physical == existing+i;
            if (contiguous) target = existing;
            else {
                // Unpinned cells may be displaced. Find a run with no pins.
                std::size_t run=0;
                for (std::size_t i=0; i<capacity(); ++i) {
                    run = pins_[i] ? 0 : run+1;
                    if (run == count) { target=i+1-count; break; }
                }
                if (target == none) throw PinnedRangeConflict{};
            }
        }
        // Check the entire destination before moving even one byte.
        for (std::size_t i=0; i<count; ++i)
            if (pins_[target+i] && physical_to_slot_[target+i] != order_[p+i])
                throw PinnedRangeConflict{};
        for (std::size_t i=0; i<count; ++i) {
            const auto phys = slots_[order_[p+i]].physical;
            if (phys != target+i) swap_physical(phys,target+i);
        }
        for (std::size_t i=0; i<count; ++i) ++pins_[target+i];
        leases_.push_back({target,count});
        return bytes_.data()+target;
    }

    void c_ptr_release(std::uint8_t* ptr) {
        if (!ptr) return;
        // The same starting pointer may have multiple leases. LIFO is the only
        // unambiguous release convention when the caller passes just a pointer.
        for (auto i=leases_.size(); i!=0; --i) {
            const auto lease=leases_[i-1];
            if (bytes_.data()+lease.physical != ptr) continue;
            for (std::size_t j=0; j<lease.count; ++j) --pins_[lease.physical+j];
            leases_.erase(leases_.begin()+i-1);
            return;
        }
        if constexpr (BoundsChecked) throw InvalidPointer{};
    }

private:
    struct Slot {
        std::size_t position = none;
        std::size_t physical = none;
        std::uint64_t generation = 0;
    };
    struct Lease { std::size_t physical, count; };
    const std::uint64_t owner_, address_limit_;
    const std::size_t max_leases_;
    std::uint64_t issued_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<Slot> slots_;
    std::vector<std::size_t> physical_to_slot_, pins_, order_, free_slots_;
    std::vector<Lease> leases_;

    [[nodiscard]] Address address_at(std::size_t p) const noexcept {
        if (p == size()) return end();
        const auto s=order_[p];
        return Address(owner_, slots_[s].generation, s);
    }
    [[nodiscard]] std::size_t position(Address a) const {
        if (a == end()) return size();
        if constexpr (BoundsChecked) {
            if (a.owner_ != owner_ || a.slot_ >= slots_.size()) throw InvalidAddress{};
            const auto& s=slots_[a.slot_];
            if (s.position == none || s.generation != a.generation_) throw InvalidAddress{};
        }
        return slots_[a.slot_].position;
    }
    [[nodiscard]] std::size_t range(Address a, std::size_t count) const {
        const auto p=position(a);
        if constexpr (BoundsChecked) { if (count > size()-p) throw OutOfBounds{}; }
        return p;
    }
    void refresh_positions(std::size_t from) noexcept {
        for (auto i=from; i<size(); ++i) slots_[order_[i]].position=i;
    }
    [[nodiscard]] std::uint8_t& byte_at(std::size_t p) noexcept { return bytes_[slots_[order_[p]].physical]; }
    [[nodiscard]] const std::uint8_t& byte_at(std::size_t p) const noexcept { return bytes_[slots_[order_[p]].physical]; }
    void swap_physical(std::size_t a, std::size_t b) noexcept {
        std::swap(bytes_[a],bytes_[b]);
        std::swap(physical_to_slot_[a],physical_to_slot_[b]);
        if (physical_to_slot_[a] != none) slots_[physical_to_slot_[a]].physical=a;
        if (physical_to_slot_[b] != none) slots_[physical_to_slot_[b]].physical=b;
    }
};

} // namespace pushable_memory
