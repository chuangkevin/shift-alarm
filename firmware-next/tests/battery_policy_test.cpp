#include "../main/battery_policy.h"
#include <cassert>

int main() {
    using namespace battery;
    assert(percent(0) == 0 && percent(1970) == 0 && percent(2062) == 20);
    assert(percent(2108) == 30 && percent(2430) == 100 && percent(4095) == 100);
    assert(charging_active(1));
    assert(!charging_active(0));
    State state;
    assert(should_sample(state, 0));
    record(state, 0, false);
    assert(!should_sample(state, 999) && should_sample(state, 1000));
    record(state, 1000, true, 1970);
    record(state, 2000, true, 2430);
    record(state, 3000, true, 2154);
    assert(!should_sample(state, 62999) && should_sample(state, 63000));
    int result = -1; uint32_t age = 0;
    assert(value(state, 303000, result, age) && result == 40 && age == 300);
    record(state, 303000, false);
    assert(!value(state, 303001, result, age));
    State rollover; record(rollover, UINT32_MAX - 500, true, 2246);
    assert(value(rollover, 499, result, age) && result == 60 && age == 1);
}
