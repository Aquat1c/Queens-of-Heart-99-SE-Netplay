#include "qoh99/MemoryView.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace qoh99 {

#if defined(_WIN32) && !defined(_WIN64)
static bool regionAccessible(const void* p, std::size_t n, bool wantWrite) {
    MEMORY_BASIC_INFORMATION mbi{};
    const unsigned char* cur = static_cast<const unsigned char*>(p);
    const unsigned char* end = cur + n;
    while (cur < end) {
        if (!VirtualQuery(cur, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
            return false;
        if (wantWrite && (prot == PAGE_READONLY || prot == PAGE_EXECUTE ||
                          prot == PAGE_EXECUTE_READ))
            return false;
        cur = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool LiveMemoryView::read(GameAddr addr, void* out, std::size_t n) const {
    const void* p = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(addr));
    if (!n || !regionAccessible(p, n, false))
        return false;
    std::memcpy(out, p, n);
    return true;
}

bool LiveMemoryView::write(GameAddr addr, const void* src, std::size_t n) {
    void* p = reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr));
    if (!n || !regionAccessible(p, n, true))
        return false;
    std::memcpy(p, src, n);
    return true;
}
#else
bool LiveMemoryView::read(GameAddr, void*, std::size_t) const { return false; }
bool LiveMemoryView::write(GameAddr, const void*, std::size_t) { return false; }
#endif

BufferMemoryView::Page& BufferMemoryView::pageFor(GameAddr addr) const {
    GameAddr key = addr & ~(kPage - 1);
    auto it = pages_.find(key);
    if (it == pages_.end())
        it = pages_.emplace(key, Page(kPage, 0)).first;
    return it->second;
}

bool BufferMemoryView::hasPage(GameAddr addr) const {
    return pages_.count(addr & ~(kPage - 1)) != 0;
}

bool BufferMemoryView::read(GameAddr addr, void* out, std::size_t n) const {
    unsigned char* dst = static_cast<unsigned char*>(out);
    while (n) {
        GameAddr off = addr & (kPage - 1);
        std::size_t chunk = static_cast<std::size_t>(kPage - off);
        if (chunk > n) chunk = n;
        if (!hasPage(addr)) {
            if (strict_) return false;
            std::memset(dst, 0, chunk);
        } else {
            std::memcpy(dst, pageFor(addr).data() + off, chunk);
        }
        addr += static_cast<GameAddr>(chunk);
        dst += chunk;
        n -= chunk;
    }
    return true;
}

bool BufferMemoryView::write(GameAddr addr, const void* src, std::size_t n) {
    const unsigned char* s = static_cast<const unsigned char*>(src);
    while (n) {
        GameAddr off = addr & (kPage - 1);
        std::size_t chunk = static_cast<std::size_t>(kPage - off);
        if (chunk > n) chunk = n;
        std::memcpy(pageFor(addr).data() + off, s, chunk);
        addr += static_cast<GameAddr>(chunk);
        s += chunk;
        n -= chunk;
    }
    return true;
}

}
