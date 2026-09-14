#include "alarm_tailnet_retry.h"
#include <assert.h>

int main(void) {
    assert(alarm_tailnet_retry_seconds(0) == 0);
    assert(alarm_tailnet_retry_seconds(1) == 1);
    assert(alarm_tailnet_retry_seconds(2) == 2);
    assert(alarm_tailnet_retry_seconds(5) == 16);
    assert(alarm_tailnet_retry_seconds(6) == 30);
    assert(alarm_tailnet_retry_seconds(255) == 30);
}
