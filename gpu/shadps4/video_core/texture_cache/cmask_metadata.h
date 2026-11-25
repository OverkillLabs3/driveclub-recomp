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
// `replace_kind`: metadata of another kind at the address is replaced, and its kind returned (the game reuses the
// address: a depth target's HTILE where a colour target's CMASK was registered). Without it the entry kept its first
// kind, and the HTILE's clears found nothing to clear there (Clear refuses a revoked CMASK entry): the depth target
// was never cleared again (DriveClub in VR at the larger eye sizes: the mirror's scene, its world went black).
template <class Mutex, class Map, class Address, class Entry, class Kind>
std::optional<Kind> Register(Mutex& mutex, Map& entries, Address address, Entry initial,
                             Kind cmask, Kind revoked, bool replace_kind = false) {
    std::scoped_lock lock{mutex};
    const auto [it, inserted] = entries.emplace(address, initial);
    if (inserted) {
        return std::nullopt;
    }
    if (initial.type == cmask && Value(it).type == revoked) {
        Value(it).type = cmask;
        // Restoring CMask intentionally preserves the existing clear_mask.
        return std::nullopt;
    }
    if (replace_kind && Value(it).type != initial.type) {
        const Kind old = Value(it).type;
        Value(it) = initial;
        return old;
    }
    return std::nullopt;
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
