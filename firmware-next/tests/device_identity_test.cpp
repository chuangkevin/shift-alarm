#include "../main/device_identity.h"
#include <cassert>
#include <cstring>

int main() {
  const uint8_t mac[6] = {0xfc, 0x01, 0x2c, 0xc9, 0x9c, 0xa8};
  char suffix[5] = {};
  deviceidentity::suffix(mac, suffix);
  assert(strcmp(suffix, "9CA8") == 0);
}
