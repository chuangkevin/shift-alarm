#include "../main/connectivity_policy.h"
#include <cassert>
#include <cstdint>

int main() {
    using connectivity::backend_reachable;
    using connectivity::should_poll_backend;
    assert(!should_poll_backend(false, true, true, true));
    assert(!should_poll_backend(true, false, true, true));
    assert(!should_poll_backend(true, true, false, true));
    assert(!should_poll_backend(true, true, true, false));
    assert(should_poll_backend(true, true, true, true));
    assert(!backend_reachable(false, true, true, true, 1000, 900));
    assert(!backend_reachable(true, false, true, true, 1000, 900));
    assert(!backend_reachable(true, true, false, true, 1000, 900));
    assert(!backend_reachable(true, true, true, false, 1000, 900));
    assert(backend_reachable(true, true, true, true, 30900, 1000));
    assert(!backend_reachable(true, true, true, true, 31001, 1000));
    const uint32_t before_wrap = UINT32_MAX - 10000;
    assert(backend_reachable(true, true, true, true, 9999, before_wrap));
    assert(!backend_reachable(true, true, true, true, 20001, before_wrap));
}
