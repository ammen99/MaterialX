#pragma once

#ifdef _WIN32
    #include <cstdint>
    using uint = uint32_t; // Define `uint` for compatibility
    #include <windows.h>

#else
    #include <sys/mman.h>
    #include <unistd.h>

#endif