#pragma once

namespace ota_manifest {
template <typename JsonValue>
inline bool available(const JsonValue &value) {
    return value.template is<bool>() && value.template as<bool>();
}
}
