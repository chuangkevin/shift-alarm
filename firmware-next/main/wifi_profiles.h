#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wifiprofiles {

constexpr size_t MAX_PROFILES = 4;
constexpr size_t MAX_SSID_BYTES = 32;
constexpr size_t MAX_PASSWORD_BYTES = 63;
constexpr uint8_t SCHEMA = 3;
constexpr size_t HEADER_SIZE = 10;
constexpr size_t CHECKSUM_SIZE = 4;
constexpr size_t MAX_BLOB_SIZE = HEADER_SIZE + MAX_PROFILES * (2 + MAX_SSID_BYTES + MAX_PASSWORD_BYTES) + CHECKSUM_SIZE;
constexpr size_t SELECTOR_SIZE = 14;
constexpr size_t LEGACY_HEADER_SIZE = 5;
constexpr size_t LEGACY_MAX_BLOB_SIZE = LEGACY_HEADER_SIZE + MAX_SSID_BYTES + MAX_PASSWORD_BYTES;

using WipeObserver = void (*)(size_t);
inline WipeObserver wipe_observer = nullptr;

inline void secureWipe(void *memory, size_t size) {
  const size_t original = size;
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
  while (size--) *bytes++ = 0;
  if (wipe_observer) wipe_observer(original);
}

template <size_t Capacity>
struct SensitiveText {
  std::array<char, Capacity + 1> bytes{};
  uint8_t length = 0;

  SensitiveText() = default;
  SensitiveText(const char *value) { assign(value, value ? strlen(value) : 0); }
  SensitiveText(const char *value, size_t size) { assign(value, size); }
  SensitiveText(const SensitiveText &other) { assign(other.c_str(), other.size()); }
  SensitiveText &operator=(const SensitiveText &other) {
    if (this != &other) { clear(); assign(other.c_str(), other.size()); }
    return *this;
  }
  SensitiveText(SensitiveText &&other) noexcept { assign(other.c_str(), other.size()); other.clear(); }
  SensitiveText &operator=(SensitiveText &&other) noexcept {
    if (this != &other) { clear(); assign(other.c_str(), other.size()); other.clear(); }
    return *this;
  }
  ~SensitiveText() { clear(); }

  bool assign(const char *value, size_t size) {
    clear();
    if (!value || size > Capacity || (size && memchr(value, 0, size))) return false;
    if (size) memcpy(bytes.data(), value, size);
    length = uint8_t(size); bytes[size] = 0; return true;
  }
  void clear() { secureWipe(bytes.data(), bytes.size()); length = 0; }
  const char *c_str() const { return bytes.data(); }
  size_t size() const { return length; }
  bool equals(const SensitiveText &other) const {
    return length == other.length && memcmp(bytes.data(), other.bytes.data(), length) == 0;
  }
  bool equals(const char *value, size_t size) const {
    return value && length == size && memcmp(bytes.data(), value, size) == 0;
  }
};

template <size_t Capacity>
struct SensitiveBytes {
  std::array<uint8_t, Capacity> bytes{};
  ~SensitiveBytes() { secureWipe(bytes.data(), bytes.size()); }
  uint8_t *data() { return bytes.data(); }
  const uint8_t *data() const { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

struct Profile {
  SensitiveText<MAX_SSID_BYTES> ssid;
  SensitiveText<MAX_PASSWORD_BYTES> password;
  Profile() = default;
  Profile(const char *name, const char *secret) : ssid(name), password(secret) {}
  Profile(const char *name, size_t name_size, const char *secret, size_t secret_size)
      : ssid(name, name_size), password(secret, secret_size) {}
};

struct List {
  std::array<Profile, MAX_PROFILES> profiles{};
  size_t count = 0;
};

struct Record {
  List list;
  uint32_t generation = 0;
};

struct Selector {
  uint8_t slot = 0;
  uint32_t generation = 0;
};

enum class Result : uint8_t { Ok, Invalid, Full, NotFound };
enum class LoadResult : uint8_t { Ok, Empty, StorageFault };
enum class CommitResult : uint8_t { Committed, CommittedStorageFault, Failed, StorageFault };

inline bool valid(const Profile &profile) {
  const size_t ssid_size = profile.ssid.size(), password_size = profile.password.size();
  return ssid_size >= 1 && ssid_size <= MAX_SSID_BYTES && password_size <= MAX_PASSWORD_BYTES &&
         (password_size == 0 || password_size >= 8);
}

inline bool decodeLegacy(const uint8_t *blob, size_t length, Profile &profile) {
  if (!blob || length < LEGACY_HEADER_SIZE || length > LEGACY_MAX_BLOB_SIZE || blob[0] != 'W' || blob[1] != 'F' ||
      blob[2] != 1 || blob[3] == 0 || blob[3] > MAX_SSID_BYTES || blob[4] > MAX_PASSWORD_BYTES ||
      length != LEGACY_HEADER_SIZE + blob[3] + blob[4]) return false;
  Profile decoded(reinterpret_cast<const char *>(blob + LEGACY_HEADER_SIZE), blob[3],
                  reinterpret_cast<const char *>(blob + LEGACY_HEADER_SIZE + blob[3]), blob[4]);
  if (!valid(decoded)) return false;
  profile = decoded; return true;
}

inline bool sameList(const List &a, const List &b) {
  if (a.count != b.count) return false;
  for (size_t i = 0; i < a.count; ++i)
    if (!a.profiles[i].ssid.equals(b.profiles[i].ssid) || !a.profiles[i].password.equals(b.profiles[i].password)) return false;
  return true;
}

inline int indexOf(const List &list, const char *ssid, size_t size) {
  for (size_t i = 0; i < list.count; ++i) if (list.profiles[i].ssid.equals(ssid, size)) return int(i);
  return -1;
}

inline Result addOrUpdate(List &list, const Profile &profile) {
  if (!valid(profile)) return Result::Invalid;
  const int existing = indexOf(list, profile.ssid.c_str(), profile.ssid.size());
  if (existing >= 0) { list.profiles[size_t(existing)].password = profile.password; return Result::Ok; }
  if (list.count >= MAX_PROFILES) return Result::Full;
  list.profiles[list.count++] = profile; return Result::Ok;
}

inline Result remove(List &list, const char *ssid, size_t size) {
  if (!ssid || size < 1 || size > MAX_SSID_BYTES || memchr(ssid, 0, size)) return Result::Invalid;
  const int existing = indexOf(list, ssid, size);
  if (existing < 0) return Result::NotFound;
  list.profiles[size_t(existing)].password.clear();
  for (size_t i = size_t(existing); i + 1 < list.count; ++i) list.profiles[i] = list.profiles[i + 1];
  list.profiles[--list.count] = Profile{}; return Result::Ok;
}

inline uint32_t checksum(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & uint32_t(0u - (crc & 1u)));
  }
  return ~crc;
}

inline void put32(uint8_t *out, uint32_t value) { for (uint8_t i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i)); }
inline uint32_t get32(const uint8_t *in) { uint32_t value = 0; for (uint8_t i = 0; i < 4; ++i) value |= uint32_t(in[i]) << (8 * i); return value; }

inline size_t encode(const List &list, uint32_t generation, uint8_t *out, size_t capacity) {
  if (!generation || list.count > MAX_PROFILES || capacity < HEADER_SIZE + CHECKSUM_SIZE) return 0;
  size_t needed = HEADER_SIZE + CHECKSUM_SIZE;
  for (size_t i = 0; i < list.count; ++i) {
    if (!valid(list.profiles[i])) return 0;
    for (size_t j = 0; j < i; ++j) if (list.profiles[i].ssid.equals(list.profiles[j].ssid)) return 0;
    needed += 2 + list.profiles[i].ssid.size() + list.profiles[i].password.size();
  }
  if (needed > capacity || needed > MAX_BLOB_SIZE) return 0;
  out[0] = 'W'; out[1] = 'F'; out[2] = 'P'; out[3] = SCHEMA; out[4] = uint8_t(list.count); out[5] = 0; put32(out + 6, generation);
  size_t cursor = HEADER_SIZE;
  for (size_t i = 0; i < list.count; ++i) {
    const Profile &profile = list.profiles[i]; out[cursor++] = uint8_t(profile.ssid.size()); out[cursor++] = uint8_t(profile.password.size());
    memcpy(out + cursor, profile.ssid.c_str(), profile.ssid.size()); cursor += profile.ssid.size();
    memcpy(out + cursor, profile.password.c_str(), profile.password.size()); cursor += profile.password.size();
  }
  put32(out + cursor, checksum(out, cursor)); return cursor + CHECKSUM_SIZE;
}

inline bool decode(const uint8_t *blob, size_t length, Record &out) {
  if (!blob || length < HEADER_SIZE + CHECKSUM_SIZE || length > MAX_BLOB_SIZE || blob[0] != 'W' || blob[1] != 'F' ||
      blob[2] != 'P' || blob[3] != SCHEMA || blob[4] > MAX_PROFILES || blob[5] != 0) return false;
  const size_t data_length = length - CHECKSUM_SIZE; const uint32_t generation = get32(blob + 6);
  if (!generation || get32(blob + data_length) != checksum(blob, data_length)) return false;
  Record decoded; decoded.generation = generation; size_t cursor = HEADER_SIZE;
  for (size_t i = 0; i < blob[4]; ++i) {
    if (cursor + 2 > data_length) return false;
    const size_t ssid_size = blob[cursor++], password_size = blob[cursor++];
    if (cursor + ssid_size + password_size > data_length) return false;
    Profile profile(reinterpret_cast<const char *>(blob + cursor), ssid_size,
                    reinterpret_cast<const char *>(blob + cursor + ssid_size), password_size);
    cursor += ssid_size + password_size;
    if (!valid(profile) || indexOf(decoded.list, profile.ssid.c_str(), profile.ssid.size()) >= 0) return false;
    decoded.list.profiles[decoded.list.count++] = profile;
  }
  if (cursor != data_length) return false;
  out = decoded; return true;
}

inline size_t encodeSelector(const Selector &selector, uint8_t out[SELECTOR_SIZE]) {
  if (selector.slot > 1 || !selector.generation) return 0;
  out[0] = 'W'; out[1] = 'F'; out[2] = 'S'; out[3] = 1; out[4] = selector.slot; out[5] = 0; put32(out + 6, selector.generation);
  put32(out + 10, checksum(out, 10));
  return SELECTOR_SIZE;
}

inline bool decodeSelector(const uint8_t *blob, size_t length, Selector &out) {
  if (!blob || length != SELECTOR_SIZE || blob[0] != 'W' || blob[1] != 'F' || blob[2] != 'S' || blob[3] != 1 || blob[4] > 1 || blob[5] != 0) return false;
  if (get32(blob + 10) != checksum(blob, 10)) return false;
  out = {blob[4], get32(blob + 6)}; return out.generation != 0;
}

struct Storage {
  virtual ~Storage() = default;
  enum class ReadResult : uint8_t { Ok, NotFound, Error };
  virtual ReadResult selectorSize(size_t &length) = 0;
  virtual ReadResult selectorData(uint8_t *out, size_t &length) = 0;
  virtual ReadResult slotSize(uint8_t slot, size_t &length) = 0;
  virtual ReadResult slotData(uint8_t slot, uint8_t *out, size_t &length) = 0;
  virtual bool writeSelector(const uint8_t *data, size_t length) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t *data, size_t length) = 0;
  virtual bool eraseSlot(uint8_t slot) = 0;
};

inline Storage::ReadResult readSelector(Storage &storage, uint8_t *out, size_t &capacity) {
  size_t stored = 0; const auto queried = storage.selectorSize(stored);
  if (queried != Storage::ReadResult::Ok) { capacity = 0; return queried; }
  if (!stored || stored > capacity) { capacity = 0; return Storage::ReadResult::Error; }
  size_t returned = stored; const auto read = storage.selectorData(out, returned);
  if (read != Storage::ReadResult::Ok || returned != stored) { capacity = 0; return Storage::ReadResult::Error; }
  capacity = returned; return Storage::ReadResult::Ok;
}

inline Storage::ReadResult readSlot(Storage &storage, uint8_t slot, uint8_t *out, size_t &capacity) {
  size_t stored = 0; const auto queried = storage.slotSize(slot, stored);
  if (queried != Storage::ReadResult::Ok) { capacity = 0; return queried; }
  if (!stored || stored > capacity) { capacity = 0; return Storage::ReadResult::Error; }
  size_t returned = stored; const auto read = storage.slotData(slot, out, returned);
  if (read != Storage::ReadResult::Ok || returned != stored) { capacity = 0; return Storage::ReadResult::Error; }
  capacity = returned; return Storage::ReadResult::Ok;
}

inline LoadResult load(Storage &storage, Record &record, Selector &selector) {
  uint8_t selector_blob[SELECTOR_SIZE]{}; size_t selector_length = sizeof(selector_blob); Selector selected;
  const auto selector_read = readSelector(storage, selector_blob, selector_length);
  const bool selector_valid = selector_read == Storage::ReadResult::Ok && decodeSelector(selector_blob, selector_length, selected);
  secureWipe(selector_blob, sizeof(selector_blob));
  if (selector_read == Storage::ReadResult::NotFound) return LoadResult::Empty;
  if (selector_read == Storage::ReadResult::Error) return LoadResult::StorageFault;
  if (!selector_valid) return LoadResult::StorageFault;
  uint8_t slot_blob[MAX_BLOB_SIZE]{}; size_t slot_length = sizeof(slot_blob); Record selected_record;
  const bool slot_valid = readSlot(storage, selected.slot, slot_blob, slot_length) == Storage::ReadResult::Ok && decode(slot_blob, slot_length, selected_record) && selected_record.generation == selected.generation;
  secureWipe(slot_blob, sizeof(slot_blob));
  if (!slot_valid) return LoadResult::StorageFault;
  record = selected_record; selector = selected; return LoadResult::Ok;
}

inline CommitResult commit(Storage &storage, const List &next, const Selector *current, Selector &committed) {
  const uint32_t generation = current ? current->generation + 1 : 1;
  if (!generation) return CommitResult::StorageFault;
  const uint8_t inactive = current ? uint8_t(current->slot ^ 1u) : 0;
  uint8_t slot_blob[MAX_BLOB_SIZE]{}; const size_t slot_length = encode(next, generation, slot_blob, sizeof(slot_blob));
  if (!slot_length || !storage.writeSlot(inactive, slot_blob, slot_length)) { secureWipe(slot_blob, sizeof(slot_blob)); return CommitResult::Failed; }
  uint8_t readback[MAX_BLOB_SIZE]{}; size_t readback_length = sizeof(readback); Record verified;
  const bool slot_verified = readSlot(storage, inactive, readback, readback_length) == Storage::ReadResult::Ok && readback_length == slot_length &&
      memcmp(slot_blob, readback, slot_length) == 0 && decode(readback, readback_length, verified) && verified.generation == generation && sameList(verified.list, next);
  secureWipe(slot_blob, sizeof(slot_blob)); secureWipe(readback, sizeof(readback));
  if (!slot_verified) return CommitResult::Failed;
  const Selector next_selector{inactive, generation}; uint8_t selector_blob[SELECTOR_SIZE]{};
  encodeSelector(next_selector, selector_blob);
  if (!storage.writeSelector(selector_blob, sizeof(selector_blob))) { secureWipe(selector_blob, sizeof(selector_blob)); return CommitResult::StorageFault; }
  uint8_t selector_readback[SELECTOR_SIZE]{}; size_t selector_length = sizeof(selector_readback); Selector decoded;
  const bool selector_verified = readSelector(storage, selector_readback, selector_length) == Storage::ReadResult::Ok && selector_length == sizeof(selector_blob) &&
      memcmp(selector_blob, selector_readback, sizeof(selector_blob)) == 0 && decodeSelector(selector_readback, selector_length, decoded) &&
      decoded.slot == next_selector.slot && decoded.generation == next_selector.generation;
  secureWipe(selector_blob, sizeof(selector_blob)); secureWipe(selector_readback, sizeof(selector_readback));
  if (!selector_verified) return CommitResult::StorageFault;
  committed = next_selector;
  if (current) {
    if (!storage.eraseSlot(current->slot)) return CommitResult::CommittedStorageFault;
    uint8_t erased[MAX_BLOB_SIZE]{}; size_t erased_length = sizeof(erased);
    const bool erased_verified = readSlot(storage, current->slot, erased, erased_length) == Storage::ReadResult::NotFound;
    secureWipe(erased, sizeof(erased));
    if (!erased_verified) return CommitResult::CommittedStorageFault;
  }
  return CommitResult::Committed;
}

}  // namespace wifiprofiles
