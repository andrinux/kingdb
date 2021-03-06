// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include "interface/database.h"

namespace kdb {

Status Database::GetRaw(ReadOptions& read_options,
                        ByteArray& key,
                        ByteArray* value_out,
                        bool want_raw_data) {
  if (is_closed_) return Status::IOError("The database is not open");
  log::trace("Database GetRaw()", "[%s]", key.ToString().c_str());
  Status s = wb_->Get(read_options, key, value_out);
  if (s.IsDeleteOrder()) {
    return Status::NotFound("Unable to find entry");
  } else if (s.IsNotFound()) {
    log::trace("Database GetRaw()", "not found in buffer");
    s = se_->Get(read_options, key, value_out);
    if (s.IsNotFound()) {
      log::trace("Database GetRaw()", "not found in storage engine");
      return s;
    } else if (s.IsOK()) {
      log::trace("Database GetRaw()", "found in storage engine");
    } else {
      log::trace("Database GetRaw()", "unidentified error");
      return s;
    }
  } else {
    log::trace("Database GetRaw()", "found in buffer");
  }

  // TODO-36: There is technical debt here:
  // 1. The uncompression should be able to proceed without having to call a
  //    Multipart Reader.
  // 2. The uncompression should be able to operate within a single buffer, and
  //    not have to copy data into intermediate buffers through the Multipart
  //    Reader as it is done here. Having intermediate buffers means that there
  //    is more data copy than necessary, thus more time wasted
  /*
  log::trace("Database GetRaw()", "Before Multipart - want_raw_data:%d value_out->is_compressed():%d", want_raw_data, value_out->is_compressed());
  if (want_raw_data == false && value_out->is_compressed()) {
    if (value_out->size() > db_options_.internal__size_multipart_required) {
      return Status::MultipartRequired();
    }
    char* buffer = new char[value_out->size()];
    uint64_t offset = 0;
    MultipartReader mp_reader(read_options, *value_out);
    for (mp_reader.Begin(); mp_reader.IsValid(); mp_reader.Next()) {
      ByteArray part;
      mp_reader.GetPart(&part);
      log::trace("Database GetRaw()", "Multipart loop size:%d [%s]", part.size(), part.ToString().c_str());
      memcpy(buffer + offset, part.data(), part.size());
      offset += part.size();
    }
    *value_out = NewShallowCopyByteArray(buffer, value_out->size());
  }
  */
  if (want_raw_data == false && value_out->is_compressed()) {
    if (value_out->size() > db_options_.internal__size_multipart_required) {
      return Status::MultipartRequired();
    }
    ByteArray value_out_uncompressed;
    compressor_.ResetThreadLocalStorage();
    s = compressor_.UncompressByteArray(*value_out,
                                        read_options.verify_checksums,
                                        &value_out_uncompressed);
    //if (!s.IsOK()) {
    //  fprintf(stderr, "Error in Get(): %s\n", s.ToString().c_str());
    //}
    *value_out = value_out_uncompressed;
  }
  return s;
}

Status Database::Get(ReadOptions& read_options, ByteArray& key, ByteArray* value_out) {
  return GetRaw(read_options, key, value_out, false);
}


Status Database::Put(WriteOptions& write_options, ByteArray& key, ByteArray& chunk) {
  return PutPart(write_options, key, chunk, 0, chunk.size());
}


Status Database::PutPart(WriteOptions& write_options,
                        ByteArray& key,
                        ByteArray& chunk,
                        uint64_t offset_chunk,
                        uint64_t size_value) {
  if (is_closed_) return Status::IOError("The database is not open");

  if (offset_chunk + chunk.size() > size_value) {
    return Status::IOError("Attempted write beyond the total value size, aborting write.");
  }

  if (size_value <= db_options_.storage__maximum_part_size) {
    return PutPartValidSize(write_options, key, chunk, offset_chunk, size_value);
  }

  // 'chunk' may be deleted by the call to PutPartValidSize()
  // and therefore it cannot be used in the loop test condition
  uint64_t size_chunk = chunk.size(); 
  Status s;
  for (uint64_t offset = 0; offset < size_chunk; offset += db_options_.storage__maximum_part_size) {
    ByteArray key_new, chunk_new;
    if (offset + db_options_.storage__maximum_part_size < chunk.size()) {
      chunk_new = chunk;
      chunk_new.set_offset(offset);
      chunk_new.set_size(db_options_.storage__maximum_part_size);
      key_new = key;
    } else {
      chunk_new = chunk;
      chunk_new.set_offset(offset);
      chunk_new.set_size(size_chunk - offset);
      key_new = key;
    }

    s = PutPartValidSize(write_options, key_new, chunk_new, offset_chunk + offset, size_value);
    if (!s.IsOK()) break;
  }

  return s;
}


Status Database::PutPartValidSize(WriteOptions& write_options,
                                 ByteArray& key,
                                 ByteArray& chunk,
                                 uint64_t offset_chunk,
                                 uint64_t size_value) {
  if (is_closed_) return Status::IOError("The database is not open");
  Status s;
  s = se_->FileSystemStatus();
  if (!s.IsOK()) return s;
  log::trace("Database::PutPartValidSize()",
            "[%s] size_chunk:%" PRIu64 " offset_chunk:%" PRIu64,
            key.ToString().c_str(),
            chunk.size(),
            offset_chunk);

  bool do_compression = true;
  uint64_t size_value_compressed = 0;
  uint64_t offset_chunk_compressed = offset_chunk;
  ByteArray chunk_final;

  bool is_first_part = (offset_chunk == 0);
  bool is_last_part = (chunk.size() + offset_chunk == size_value);
  log::trace("Database::PutPartValidSize()",
            "CompressionType:%d",
            db_options_.compression.type);

  if (   chunk.size() == 0
      || db_options_.compression.type == kNoCompression) {
    do_compression = false;
  }

  if (is_first_part) {
    ts_compression_enabled_.put(1);
    ts_offset_.put(0);
  }

  if (ts_compression_enabled_.get() == 0) {
    // If compression is disabled, chunks are copied uncompressed, but the first
    // of the chunk copied when compression was disabled was shifted to have a
    // frame header, thus the current offset needs to account for it.
    //offset_chunk_compressed += compressor_.size_frame_header();
    offset_chunk_compressed = ts_offset_.get();
    ts_offset_.put(offset_chunk_compressed + chunk.size());
  }

  if (!do_compression || ts_compression_enabled_.get() == 0) {
    chunk_final = chunk;
  } else {
    std::chrono::high_resolution_clock::time_point step00 = std::chrono::high_resolution_clock::now();
    if (is_first_part) {
      compressor_.ResetThreadLocalStorage();
    }
    std::chrono::high_resolution_clock::time_point step01 = std::chrono::high_resolution_clock::now();

    offset_chunk_compressed = compressor_.size_compressed();
    uint64_t size_compressed;
    char *compressed;
    s = compressor_.Compress(chunk.data(),
                             chunk.size(),
                             &compressed,
                             &size_compressed);
    if (!s.IsOK()) return s;
    std::chrono::high_resolution_clock::time_point step02 = std::chrono::high_resolution_clock::now();

    log::trace("Database::PutPartValidSize()",
              "[%s] size_compressed:%" PRIu64,
              key.ToString().c_str(), compressor_.size_compressed());

    // Now Checking if compression should be disabled for this entry
    uint64_t size_remaining = size_value - offset_chunk;
    uint64_t space_left = size_value + EntryHeader::CalculatePaddingSize(size_value) - offset_chunk_compressed;
    if (  size_remaining - chunk.size() + compressor_.size_frame_header()
        > space_left - size_compressed) {
      delete[] compressed;
      compressed = new char[compressor_.size_uncompressed_frame(chunk.size())];
      compressor_.DisableCompressionInFrameHeader(compressed);
      memcpy(compressed + compressor_.size_frame_header(), chunk.data(), chunk.size());
      compressor_.AdjustCompressedSize(- size_compressed);
      size_compressed = chunk.size() + compressor_.size_frame_header();
      ts_compression_enabled_.put(0);
      ts_offset_.put(compressor_.size_compressed() + size_compressed);
    }
    std::chrono::high_resolution_clock::time_point step03 = std::chrono::high_resolution_clock::now();

    ByteArray chunk_compressed = NewShallowCopyByteArray(compressed, size_compressed);
    std::chrono::high_resolution_clock::time_point step04 = std::chrono::high_resolution_clock::now();

    log::trace("Database::PutPartValidSize()",
              "[%s] (%" PRIu64 ") compressed size %" PRIu64 " - offset_chunk_compressed %" PRIu64,
              key.ToString().c_str(),
              chunk.size(),
              chunk_compressed.size(),
              offset_chunk_compressed);

    chunk_final = chunk_compressed;
    std::chrono::high_resolution_clock::time_point step05 = std::chrono::high_resolution_clock::now();
    uint64_t duration00 = std::chrono::duration_cast<std::chrono::microseconds>(step01 - step00).count();
    uint64_t duration01 = std::chrono::duration_cast<std::chrono::microseconds>(step02 - step01).count();
    uint64_t duration02 = std::chrono::duration_cast<std::chrono::microseconds>(step03 - step02).count();
    uint64_t duration03 = std::chrono::duration_cast<std::chrono::microseconds>(step04 - step03).count();
    uint64_t duration04 = std::chrono::duration_cast<std::chrono::microseconds>(step05 - step04).count();

    /*
    log::info("Database::PutPartValidSize()",
              "Durations: [%" PRIu64 "] [%" PRIu64 "] [%" PRIu64 "] [%" PRIu64 "] [%" PRIu64 "]",
              duration00, duration01, duration02, duration03, duration04
             );
    */
  }

  if (do_compression && is_last_part) {
    if (ts_compression_enabled_.get() == 1) {
      size_value_compressed = compressor_.size_compressed();
    } else {
      if (is_first_part) {
        // chunk is self-contained: first ans last
        size_value_compressed = ts_offset_.get();
      } else {
        size_value_compressed = offset_chunk_compressed + chunk.size();
      }
    }
  }

  // Compute CRC32 checksum
  uint32_t crc32 = 0;
  if (is_first_part) {
    crc32_.ResetThreadLocalStorage();
    crc32_.stream(key.data(), key.size());
  }
  crc32_.stream(chunk_final.data(), chunk_final.size());
  if (is_last_part) crc32 = crc32_.get();

  log::trace("Database PutPartValidSize()", "[%s] size_value_compressed:%" PRIu64 " crc32:0x%" PRIx64 " END", key.ToString().c_str(), size_value_compressed, crc32);

  uint64_t size_padding = do_compression ? EntryHeader::CalculatePaddingSize(size_value) : 0;
  if (  offset_chunk_compressed + chunk_final.size()
      > size_value + size_padding) {
    log::emerg("Database::PutPartValidSize()", "Error: write was attempted outside of the allocated memory.");
    return Status::IOError("Prevented write to occur outside of the allocated memory.");
  }

  // (size_value_compressed != 0 && chunk->size() + offset_chunk == size_value_compressed));
  return wb_->PutPart(write_options,
                       key,
                       chunk_final,
                       offset_chunk_compressed,
                       size_value,
                       size_value_compressed,
                       crc32);
}


Status Database::Delete(WriteOptions& write_options,
                        ByteArray& key) {
  if (is_closed_) return Status::IOError("The database is not open");
  log::trace("Database::Delete()", "[%s]", key.ToString().c_str());
  Status s = se_->FileSystemStatus();
  if (!s.IsOK()) return s;
  return wb_->Delete(write_options, key);
}


void Database::Flush() {
  wb_->Flush();
}


void Database::Compact() {
  wb_->Flush();
  se_->FlushCurrentFileForForcedCompaction();
  se_->Compact();
}


Snapshot Database::NewSnapshot() {
  if (is_closed_) return Snapshot();
  log::trace("Database::NewSnapshot()", "start");

  wb_->Flush();
  uint32_t fileid_end = se_->FlushCurrentFileForSnapshot();

  std::set<uint32_t>* fileids_ignore;
  uint32_t snapshot_id;
  Status s = se_->GetNewSnapshotData(&snapshot_id, &fileids_ignore);
  if (!s.IsOK()) return Snapshot();

  StorageEngine *se_readonly = new StorageEngine(db_options_,
                                                 nullptr,
                                                 dbname_,
                                                 true,
                                                 fileids_ignore,
                                                 fileid_end);
  std::vector<uint32_t> *fileids_iterator = se_readonly->GetFileidsIterator();
  Snapshot snapshot(db_options_,
                    dbname_,
                    se_,
                    se_readonly,
                    fileids_iterator,
                    snapshot_id);
  return snapshot;
}


KingDB* Database::NewSnapshotPointer() {
  if (is_closed_) return nullptr;
  log::trace("Database::NewSnapshotPointer()", "start");

  wb_->Flush();
  uint32_t fileid_end = se_->FlushCurrentFileForSnapshot();

  std::set<uint32_t>* fileids_ignore;
  uint32_t snapshot_id;
  Status s = se_->GetNewSnapshotData(&snapshot_id, &fileids_ignore);
  if (!s.IsOK()) return nullptr;

  StorageEngine *se_readonly = new StorageEngine(db_options_,
                                                 nullptr,
                                                 dbname_,
                                                 true,
                                                 fileids_ignore,
                                                 fileid_end);
  std::vector<uint32_t> *fileids_iterator = se_readonly->GetFileidsIterator();
  Snapshot *snapshot = new Snapshot(db_options_,
                                    dbname_,
                                    se_,
                                    se_readonly,
                                    fileids_iterator,
                                    snapshot_id);
  return snapshot;
}




Iterator Database::NewIterator(ReadOptions& read_options) {
  if (is_closed_) return Iterator();
  KingDB* snapshot = NewSnapshotPointer();
  Iterator it = snapshot->NewIterator(read_options);
  //Iterator *si = static_cast<BasicIterator*>(it);
  it.SetParentSnapshot(snapshot);
  return it;
}

} // namespace kdb
