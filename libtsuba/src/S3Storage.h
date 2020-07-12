#ifndef GALOIS_LIBTSUBA_S3_STORAGE_H_
#define GALOIS_LIBTSUBA_S3_STORAGE_H_

#include <cstdint>

#include "galois/Result.h"
#include "FileStorage.h"

namespace tsuba {

class S3Storage : public FileStorage {
  friend class GlobalState;
  galois::Result<std::pair<std::string, std::string>>
  CleanURI(const std::string& uri);

  S3Storage() : FileStorage("s3://") {}

public:
  galois::Result<void> Init() override;
  galois::Result<void> Fini() override;
  galois::Result<void> Stat(const std::string& uri, StatBuf* s_buf) override;
  galois::Result<void> Create(const std::string& uri, bool overwrite) override;

  galois::Result<void> GetMultiSync(const std::string& uri, uint64_t start,
                                    uint64_t size,
                                    uint8_t* result_buf) override;

  galois::Result<void> PutMultiSync(const std::string& uri, const uint8_t* data,
                                    uint64_t size) override;

  // Call these functions in order to do an async multipart put
  // All but the first call can block, making this a bulk synchronous parallel
  // interface
  galois::Result<void> PutMultiAsync1(const std::string& uri,
                                      const uint8_t* data,
                                      uint64_t size) override;
  galois::Result<void> PutMultiAsync2(const std::string& uri) override;
  galois::Result<void> PutMultiAsync3(const std::string& uri) override;
  galois::Result<void> PutMultiAsyncFinish(const std::string& uri) override;
  galois::Result<void> PutSingleSync(const std::string& uri,
                                     const uint8_t* data,
                                     uint64_t size) override;

  galois::Result<void> PutSingleAsync(const std::string& uri,
                                      const uint8_t* data,
                                      uint64_t size) override;

  galois::Result<void> PutSingleAsyncFinish(const std::string& uri) override;
};

} // namespace tsuba

#endif