#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
namespace BbCmaskMetadata {
template <class Iterator> decltype(auto) Value(Iterator& it) {
    if constexpr (requires { it.value(); }) {
        return (it.value());
    } else {
        return (it->second);
    }
}
// Every function below is a leaf: registry operations only, no callers' locks,
// scheduler, image traversal, logging, or external callbacks under this mutex.
template <class Mutex, class Map, class Address, class Kind>
std::optional<Kind> Query(Mutex& mutex, const Map& entries, Address address,
                          Kind revoked) {
    std::scoped_lock lock{mutex};
    const auto it = entries.find(address);
    if (it == entries.end() || Value(it).type == revoked) {
        return std::nullopt;
    }
    return Value(it).type;
}
template <class Mutex, class Map, class Address, class Kind>
bool Cleared(Mutex& mutex, const Map& entries, Address address, unsigned slice,
             Kind revoked) {
    std::scoped_lock lock{mutex};
    const auto it = entries.find(address);
    return it != entries.end() && Value(it).type != revoked &&
           (Value(it).clear_mask & (1u << slice));
}
template <class Mutex, class Map, class Address, class Kind>
bool Clear(Mutex& mutex, Map& entries, Address address, Kind revoked) {
    std::scoped_lock lock{mutex};
    const auto it = entries.find(address);
    if (it == entries.end() || Value(it).type == revoked) {
        return false;
    }
    Value(it).clear_mask = -1;
    return true;
}
template <class Mutex, class Map, class Address, class Kind>
bool Touch(Mutex& mutex, Map& entries, Address address, unsigned slice,
           bool clear, Kind revoked) {
    std::scoped_lock lock{mutex};
    const auto it = entries.find(address);
    if (it == entries.end() || Value(it).type == revoked) {
        return false;
    }
    if (clear) {
        Value(it).clear_mask |= 1u << slice;
    } else {
        Value(it).clear_mask &= ~(1u << slice);
    }
    return true;
}
template <class Mutex, class Map, class Address, class Entry, class Kind>
void Register(Mutex& mutex, Map& entries, Address address, Entry initial,
              Kind cmask, Kind revoked) {
    std::scoped_lock lock{mutex};
    const auto [it, inserted] = entries.emplace(address, initial);
    if (!inserted && initial.type == cmask && Value(it).type == revoked) {
        Value(it).type = cmask;
    }
    // Restoring CMask intentionally preserves the existing clear_mask.
}
template <class Mutex, class Map, class Addresses>
void Erase(Mutex& mutex, Map& entries, const Addresses& addresses) {
    std::scoped_lock lock{mutex};
    for (const auto address : addresses) {
        if (address) {
            entries.erase(address);
        }
    }
}
struct Revocation {
    std::size_t count = 0;
    std::uint64_t first = 0;
};
template <class Mutex, class Map, class Address, class Size, class Kind,
          class Generation>
Revocation Revoke(Mutex& mutex, Map& entries, Address begin, Size size,
                  Kind cmask, Kind revoked, Generation& generation) {
    std::scoped_lock lock{mutex};
    Revocation result;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto address = it->first;
        if (Value(it).type != cmask || address < begin ||
            address - begin >= size) {
            continue;
        }
        Value(it).type = revoked;
        if (!result.count) {
            result.first = address;
        }
        ++result.count;
    }
    if (result.count) {
        generation.fetch_add(1, std::memory_order_acq_rel);
    }
    return result;
}
} // namespace BbCmaskMetadata
