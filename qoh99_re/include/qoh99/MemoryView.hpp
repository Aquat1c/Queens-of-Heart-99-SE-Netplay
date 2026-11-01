#pragma once
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include "Types.hpp"

namespace qoh99 {

class MemoryView {
public:
    virtual ~MemoryView() = default;
    virtual bool read(GameAddr addr, void* out, std::size_t n) const = 0;
    virtual bool write(GameAddr addr, const void* src, std::size_t n) = 0;

    template <typename T>
    T get(GameAddr addr, T fallback = T{}) const {
        T v{};
        return read(addr, &v, sizeof(T)) ? v : fallback;
    }
    template <typename T>
    bool set(GameAddr addr, const T& v) { return write(addr, &v, sizeof(T)); }
};

template <typename T>
struct TypedAddress {
    GameAddr addr;
    T load(const MemoryView& m, T fallback = T{}) const { return m.get<T>(addr, fallback); }
    bool store(MemoryView& m, const T& v) const { return m.set<T>(addr, v); }
};

template <typename Signature>
struct FunctionPtr {
    GameAddr addr;
#if defined(_WIN32) && !defined(_WIN64) && defined(QOH99_LIVE_PROCESS)
    Signature* get() const { return reinterpret_cast<Signature*>(static_cast<std::uintptr_t>(addr)); }
#endif
};

class LiveMemoryView : public MemoryView {
public:
    bool read(GameAddr addr, void* out, std::size_t n) const override;
    bool write(GameAddr addr, const void* src, std::size_t n) override;
};

class BufferMemoryView : public MemoryView {
public:
    explicit BufferMemoryView(bool strict = false) : strict_(strict) {}
    bool read(GameAddr addr, void* out, std::size_t n) const override;
    bool write(GameAddr addr, const void* src, std::size_t n) override;
private:
    static constexpr GameAddr kPage = 0x1000;
    using Page = std::vector<unsigned char>;
    mutable std::map<GameAddr, Page> pages_;
    bool strict_;
    Page& pageFor(GameAddr addr) const;
    bool hasPage(GameAddr addr) const;
};

}
