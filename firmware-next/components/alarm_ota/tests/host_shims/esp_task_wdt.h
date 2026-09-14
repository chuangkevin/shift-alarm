#pragma once
#include "esp_err.h"
typedef void *esp_task_wdt_user_handle_t;
esp_err_t esp_task_wdt_add_user(const char *name, esp_task_wdt_user_handle_t *out);
esp_err_t esp_task_wdt_delete_user(esp_task_wdt_user_handle_t user);
