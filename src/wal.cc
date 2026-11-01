#include "status.h"
#include <cassert>
#include <coding.h>
#include <crc32c.h>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <string>
#include <string_view>
#include <wal.h>

namespace gritdb {
std::string EncodeRecord(RecordType type, std::string_view payload) {
  assert(payload.size() <= kBlockSize - kHeaderSize);

  std::string rec;
  rec.reserve(kHeaderSize + payload.size());
  rec.append(4, '\0');
  PutFixed16(&rec, static_cast<uint16_t>(payload.size()));
  rec.push_back(static_cast<char>(static_cast<uint8_t>(type)));
  rec.append(payload.data(), payload.size());

  const uint32_t crc = crc32c::Value(rec.data() + 4, rec.size() - 4);
  EncodeFixed32(rec.data(), crc);
  return rec;
}
Status LogWriter::AddRecord(std::string_view record) {
  if (record.size() > kBlockSize - kHeaderSize) {
    return Status::InvalidArgument("record too large for a full fragement.");
  }
  size_t leftover = kBlockSize - blockOffset;
  if (kHeaderSize + record.size() > leftover) {

    if (leftover > 0) {
      Status s = file->Append(std::string(leftover, '\0'));
      if (!s.ok()) {
        return s;
      }
    }
    blockOffset = 0;
  }
  const std::string rec = EncodeRecord(RecordType::Full, record);
  Status s = file->Append(rec);
  if (!s.ok()) {
    return s;
  }
  blockOffset += rec.size();
  return file->Flush();
}
Status LogWriter::Sync() { return file->Sync(); }
Status LogWriter::Close() { return file->Close(); }
bool LogReader::ReadRecord(std::string *record) {
  if (!curr.ok()) {
    return false;
  }
  while (true) {
    if (curr_pos >= data.size()) {
      return false;
    }
    const size_t leftover = kBlockSize - curr_pos % kBlockSize;
    if (leftover < kHeaderSize) {
      curr_pos += leftover;
      continue;
    }
    if (data.size() - curr_pos < kHeaderSize) {
      curr_pos = data.size();
      return false;
    }
    const char *p = data.data() + curr_pos;
    const uint32_t stored_crc = DecodeFixed32(p);
    const size_t length = DecodeFixed16(p + 4);
    const uint8_t type = static_cast<uint8_t>(p[6]);

    if (type == static_cast<uint8_t>(RecordType::Zero)) {
      curr_pos += leftover;
      continue;
    }

    if (type != static_cast<uint8_t>(RecordType::Full)) {
      curr = Status::Corruption("unexpected record type");
      return false;
    }
    if (kHeaderSize + length > leftover) { // writer never crosses a block
      curr = Status::Corruption("record crosses block boundary");
      return false;
    }

    const size_t end = curr_pos + kHeaderSize + length;
    if (end > data.size()) { // truncated: torn tail
      curr_pos = data.size();
      return false;
    }
    if (crc32c::Value(p + 4, kHeaderSize - 4 + length) != stored_crc) {
      if (end == data.size()) { // bad last record: torn tail
        curr_pos = data.size();
        return false;
      }
      curr = Status::Corruption("checksum mismatch");
      return false;
    }

    record->assign(p + kHeaderSize, length);
    curr_pos = end;
    return true;
  }
}
Status LogReader::status() const { return curr; }

} // namespace gritdb
