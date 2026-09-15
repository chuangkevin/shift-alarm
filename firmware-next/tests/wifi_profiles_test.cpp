#include "../main/wifi_profiles.h"
#include <array>
#include <cassert>
#include <cstring>
#include <string>

using namespace wifiprofiles;

struct FakeStorage : Storage {
  std::array<std::array<uint8_t, MAX_BLOB_SIZE>, 2> slots{};
  std::array<size_t, 2> slot_sizes{};
  std::array<uint8_t, SELECTOR_SIZE> selector{};
  size_t selector_size = 0;
  int fail_at = 0;
  int operation = 0;
  enum class ReadFault { None, SelectorSizeNotFound, SelectorSizeError, SelectorDataNotFound, SelectorDataError,
                         SlotSizeNotFound, SlotSizeError, SlotDataNotFound, SlotDataError, SelectorShortData, SlotShortData } read_fault = ReadFault::None;

  bool allowed() { return !fail_at || ++operation != fail_at; }
  ReadResult selectorSize(size_t &length) override {
    if (!allowed() || read_fault == ReadFault::SelectorSizeError) return ReadResult::Error;
    if (!selector_size || read_fault == ReadFault::SelectorSizeNotFound) return ReadResult::NotFound;
    length = selector_size; return ReadResult::Ok;
  }
  ReadResult selectorData(uint8_t *out, size_t &length) override {
    if (!allowed() || read_fault == ReadFault::SelectorDataError) return ReadResult::Error;
    if (read_fault == ReadFault::SelectorDataNotFound) return ReadResult::NotFound;
    memcpy(out, selector.data(), selector_size); length = read_fault == ReadFault::SelectorShortData ? selector_size - 1 : selector_size; return ReadResult::Ok;
  }
  ReadResult slotSize(uint8_t slot, size_t &length) override {
    if (!allowed() || read_fault == ReadFault::SlotSizeError) return ReadResult::Error;
    if (!slot_sizes[slot] || read_fault == ReadFault::SlotSizeNotFound) return ReadResult::NotFound;
    length = slot_sizes[slot]; return ReadResult::Ok;
  }
  ReadResult slotData(uint8_t slot, uint8_t *out, size_t &length) override {
    if (!allowed() || read_fault == ReadFault::SlotDataError) return ReadResult::Error;
    if (read_fault == ReadFault::SlotDataNotFound) return ReadResult::NotFound;
    memcpy(out, slots[slot].data(), slot_sizes[slot]); length = read_fault == ReadFault::SlotShortData ? slot_sizes[slot] - 1 : slot_sizes[slot]; return ReadResult::Ok;
  }
  bool writeSelector(const uint8_t *data, size_t length) override {
    if (!allowed()) return false; memcpy(selector.data(), data, length); selector_size = length; return true;
  }
  bool writeSlot(uint8_t slot, const uint8_t *data, size_t length) override {
    if (!allowed()) return false; memcpy(slots[slot].data(), data, length); slot_sizes[slot] = length; return true;
  }
  bool eraseSlot(uint8_t slot) override {
    if (!allowed()) return false; secureWipe(slots[slot].data(), slots[slot].size()); slot_sizes[slot] = 0; return true;
  }
};

static size_t wiped = 0;
static void observedWipe(size_t size) { wiped += size; }

int main() {
  assert(!valid({"", "12345678"}));
  assert(valid({"open", ""}));
  assert(!valid({"home", "1234567"}));
  Profile maximum(std::string(32, 's').c_str(), std::string(63, 'p').c_str());
  assert(valid(maximum));

  List list;
  assert(addOrUpdate(list, {"one", "12345678"}) == Result::Ok);
  assert(addOrUpdate(list, {"two", "abcdefgh"}) == Result::Ok);
  assert(addOrUpdate(list, {"one", "new-pass"}) == Result::Ok);
  assert(list.count == 2 && strcmp(list.profiles[0].password.c_str(), "new-pass") == 0);
  assert(addOrUpdate(list, {"three", ""}) == Result::Ok);
  assert(addOrUpdate(list, {"four", "password"}) == Result::Ok);
  assert(addOrUpdate(list, {"five", "password"}) == Result::Full);

  wipe_observer = observedWipe;
  Profile sensitive("wipe", "old-pass"); wiped = 0; sensitive.password.clear();
  assert(wiped == MAX_PASSWORD_BYTES + 1);
  for (char byte : sensitive.password.bytes) assert(byte == 0);
  assert(remove(list, "two", 3) == Result::Ok);
  assert(wiped >= MAX_PASSWORD_BYTES + 1);
  assert(list.count == 3 && strcmp(list.profiles[1].ssid.c_str(), "three") == 0);

  uint8_t blob[MAX_BLOB_SIZE]{};
  const size_t size = encode(list, 7, blob, sizeof(blob));
  assert(size > HEADER_SIZE + CHECKSUM_SIZE && blob[3] == SCHEMA);
  Record decoded;
  assert(decode(blob, size, decoded));
  assert(decoded.generation == 7 && sameList(decoded.list, list));
  blob[HEADER_SIZE + 2] ^= 1;
  assert(!decode(blob, size, decoded));

  FakeStorage storage;
  Record loaded; Selector active;
  assert(load(storage, loaded, active) == LoadResult::Empty);
  for (int boundary = 1; boundary <= 6; ++boundary) {
    FakeStorage migration; migration.fail_at = boundary; Selector migration_selector{};
    const CommitResult outcome = commit(migration, list, nullptr, migration_selector);
    assert(outcome == (boundary < 4 ? CommitResult::Failed : CommitResult::StorageFault));
    migration.fail_at = 0; migration.operation = 0; Record migration_record; Selector recovered_selector;
    const LoadResult recovery = load(migration, migration_record, recovered_selector);
    if (boundary < 5) assert(recovery == LoadResult::Empty);
    else assert(recovery == LoadResult::Ok && recovered_selector.generation == 1 && sameList(migration_record.list, list));
  }
  Selector committed;
  assert(commit(storage, list, nullptr, committed) == CommitResult::Committed);
  assert(committed.slot == 0 && committed.generation == 1);
  assert(load(storage, loaded, active) == LoadResult::Ok && active.generation == 1 && sameList(loaded.list, list));

  List changed = list;
  assert(addOrUpdate(changed, {"one", "changed1"}) == Result::Ok);
  assert(commit(storage, changed, &active, committed) == CommitResult::Committed);
  assert(committed.slot == 1 && committed.generation == 2);
  assert(storage.slot_sizes[active.slot] == 0);

  // Every slot/selector write/read boundary fails closed. The old selector remains authoritative
  // before selector write; selector write/read uncertainty blocks until load resolves it.
  for (int boundary = 1; boundary <= 8; ++boundary) {
    FakeStorage fault = storage; fault.fail_at = boundary; fault.operation = 0;
    Selector result{}; const CommitResult outcome = commit(fault, list, &committed, result);
    const CommitResult expected = boundary < 4 ? CommitResult::Failed : boundary < 7 ? CommitResult::StorageFault : CommitResult::CommittedStorageFault;
    assert(outcome == expected);
    Record recovery; Selector recovered;
    fault.fail_at = 0; fault.operation = 0;
    const LoadResult recovered_result = load(fault, recovery, recovered);
    if (boundary == 4) assert(recovered_result == LoadResult::Ok && recovered.generation == committed.generation);
    if (boundary >= 5) assert(recovered_result == LoadResult::Ok && recovered.generation == committed.generation + 1);
  }

  for (auto fault : {FakeStorage::ReadFault::SelectorSizeError, FakeStorage::ReadFault::SelectorDataError,
                     FakeStorage::ReadFault::SelectorDataNotFound, FakeStorage::ReadFault::SelectorShortData}) {
    FakeStorage broken = storage; broken.read_fault = fault;
    assert(load(broken, loaded, active) == LoadResult::StorageFault);
  }
  FakeStorage absent_selector = storage; absent_selector.read_fault = FakeStorage::ReadFault::SelectorSizeNotFound;
  assert(load(absent_selector, loaded, active) == LoadResult::Empty);
  for (auto fault : {FakeStorage::ReadFault::SlotSizeNotFound, FakeStorage::ReadFault::SlotSizeError,
                     FakeStorage::ReadFault::SlotDataNotFound, FakeStorage::ReadFault::SlotDataError,
                     FakeStorage::ReadFault::SlotShortData}) {
    FakeStorage broken = storage; broken.read_fault = fault;
    assert(load(broken, loaded, active) == LoadResult::StorageFault);
  }

  // Corrupt or generation-mismatched selector/slot pairs never choose a newer orphan slot.
  FakeStorage corrupt = storage; corrupt.selector[10] ^= 1;
  assert(load(corrupt, loaded, active) == LoadResult::StorageFault);
  FakeStorage missing = storage; missing.slot_sizes[committed.slot] = 0;
  assert(load(missing, loaded, active) == LoadResult::StorageFault);
  const size_t before_malformed = wiped;
  {
    SensitiveBytes<LEGACY_MAX_BLOB_SIZE> malformed;
    malformed.data()[0] = 'X';
    Profile ignored;
    assert(!decodeLegacy(malformed.data(), LEGACY_HEADER_SIZE, ignored));
  }
  assert(wiped >= before_malformed + LEGACY_MAX_BLOB_SIZE);
  const size_t before_bad_length = wiped;
  {
    SensitiveBytes<LEGACY_MAX_BLOB_SIZE> malformed;
    malformed.data()[0] = 'W'; malformed.data()[1] = 'F'; malformed.data()[2] = 1;
    malformed.data()[3] = 4; malformed.data()[4] = 8;
    Profile ignored;
    assert(!decodeLegacy(malformed.data(), LEGACY_HEADER_SIZE + 11, ignored));
  }
  assert(wiped >= before_bad_length + LEGACY_MAX_BLOB_SIZE);
  assert(wiped > MAX_BLOB_SIZE * 4);
  wipe_observer = nullptr;
}
