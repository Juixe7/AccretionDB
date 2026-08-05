#ifndef FORGELSM_IO_UTIL_H
#define FORGELSM_IO_UTIL_H

#include <cstdint>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #include <iostream>
#else
  #include <unistd.h>
  #include <errno.h>
#endif

namespace forgelsm {

inline bool platform_pread(intptr_t fd, void* buf, size_t count, uint64_t offset) {
#ifdef _WIN32
    HANDLE h = (HANDLE)fd;
    if (h == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ol = {0};
    ol.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ol.OffsetHigh = (DWORD)(offset >> 32);
    DWORD read_bytes = 0;
    bool ok = ReadFile(h, buf, static_cast<DWORD>(count), &read_bytes, &ol);
    return ok && read_bytes == count;
#else
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t rem = count;
    uint64_t cur_off = offset;
    while (rem > 0) {
        auto n = pread((int)fd, p, rem, cur_off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        cur_off += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
#endif
}

} // namespace forgelsm

#endif // FORGELSM_IO_UTIL_H
