#pragma once
#include <stdbool.h>
extern bool host_boot_selected;
extern bool host_restarted;
extern bool host_flash_read_fail;
extern bool host_descriptor_fail;
extern bool host_target_mismatch;
const char *host_test_dir(void);
