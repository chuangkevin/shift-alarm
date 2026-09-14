#include "../main/ota_manifest_policy.h"
#include <ArduinoJson.h>
#include <cassert>

int main() {
    JsonDocument document;
    document["available"] = true;
    assert(ota_manifest::available(document["available"]));
    document["available"] = false;
    assert(!ota_manifest::available(document["available"]));
    document["available"] = 1;
    assert(!ota_manifest::available(document["available"]));
    document["available"] = "true";
    assert(!ota_manifest::available(document["available"]));
    document["available"] = nullptr;
    assert(!ota_manifest::available(document["available"]));
}
