#pragma once
#include <thread>
#include <chrono>
inline int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*) {return 0;}
inline void vTaskDelete(void*) {}
inline void vTaskDelay(unsigned ms) {std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
