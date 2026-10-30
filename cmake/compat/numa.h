#pragma once

#if defined(__linux__) && defined(YLT_ENABLE_IBV)
#include_next <numa.h>
#else

inline int numa_max_node() noexcept {
    return 1;
}

inline int numa_run_on_node(int) noexcept {
    return 0;
}

inline void numa_set_preferred(int) noexcept {}

#endif
