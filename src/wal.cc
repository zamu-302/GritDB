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
  bool is_first = true;
  do {
    size_t leftover = kBlockSize - blockOffset;
    if (leftover < kHeaderSize) {
      if (leftover > 0) {

        Status s = file->Append(std::string(leftover, '\0'));
        if (!s.ok()) {
          return s;
        }
      }
      blockOffset = 0;
      leftover = kBlockSize;
    }
    const size_t avail = leftover - kHeaderSize;
    const size_t frag_len = std::min(record.size(), avail);
    const bool is_last = frag_len == record.size();

    RecordType type;
    if (is_first && is_last) {
      type = RecordType::Full;
    } else if (is_first) {
      type = RecordType::First;
    } else if (is_last) {
      type = RecordType::Last;
    } else {
      type = RecordType::Middle;
    }
    const std::string rec = EncodeRecord(type, record.substr(0, frag_len));
    Status s = file->Append(rec);
    if (!s.ok()) {
      return s;
    }
    blockOffset += rec.size();
    record.remove_prefix(frag_len);
    is_first = false;
  } while (!record.empty());
  return file->Flush();
}

Status LogWriter::Sync() { return file->Sync(); }
Status LogWriter::Close() { return file->Close(); }

bool LogReader::ReadRecord(std::string *record) {
  if (!curr.ok()) {
    return false;
  }
  std::string scratch;
  bool in_record = false;

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

    if (type < static_cast<uint8_t>(RecordType::Full) ||
        type > static_cast<uint8_t>(RecordType::Last)) {
      curr = Status::Corruption("unknown record type");
      return false;
    }
    if (kHeaderSize + length > leftover) {
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
    const std::string_view payload(p + kHeaderSize, length);
    curr_pos = end;
    switch (static_cast<RecordType>(type)) {
    case RecordType::Full:
      if (in_record) {
        curr = Status::Corruption(
            "fragment not complete 'Full' inside a split record.");
        return false;
      }
      record->assign(payload.data(), payload.size());
      return true;
    case RecordType::First:
      if (in_record) {
        curr = Status::Corruption("first fragment inside a split record.");
        return false;
      }
      scratch.append(payload.data(), payload.size());
      in_record = true;
      break;
    case RecordType::Middle:
      if (!in_record) {
        curr = Status::Corruption("Middle fragment without First.");
        return false;
      }
      scratch.append(payload.data(), payload.size());
      break;
    case RecordType::Last:
      if (!in_record) {
        curr = Status::Corruption("Last fragment without First.");
        return false;
      }
      scratch.append(payload.data(), payload.size());
      *record = std::move(scratch);
      return true;
    default:
      break;
    }
  }
}
Status LogReader::status() const { return curr; }

} // namespace gritdb
