#include "common.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h" // fixfix
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream.h"

namespace Grpc {

class BufferOutputStream : public google::protobuf::io::ZeroCopyOutputStream {
public:
  BufferOutputStream(Buffer::Instance& buffer, uint32_t size)
      : buffer_(buffer), num_iovecs_(buffer.reserve(size, iovecs_, 2)) {}

  ~BufferOutputStream() {
    ASSERT(current_iovec_ >= 0);
    buffer_.commit(iovecs_, current_iovec_ + 1);
  }

  // google::protobuf::io::ZeroCopyOutputStream
  bool Next(void** data , int* size) override {
    current_iovec_++;
    ASSERT(current_iovec_ <= num_iovecs_);
    *data = iovecs_[current_iovec_].mem_;
    *size = iovecs_[current_iovec_].len_;
    written_ += *size;
    return true;
  }

  void BackUp(int count) override {
    ASSERT(current_iovec_ >= 0);
    ASSERT(iovecs_[current_iovec_].len_ >= static_cast<uint64_t>(count));
    ASSERT(written_ >= static_cast<uint64_t>(count));
    iovecs_[current_iovec_].len_ -= count;
    written_ -= count;
  }

  int64_t ByteCount() const override {
    return written_;
  }

private:
  Buffer::Instance& buffer_;
  Buffer::RawSlice iovecs_[2];
  const int64_t num_iovecs_;
  int64_t current_iovec_{-1};
  uint64_t written_{};
};

const std::string Common::GRPC_CONTENT_TYPE{"application/grpc"};

void Common::chargeStat(const Upstream::ClusterInfo& cluster, const std::string& grpc_service,
                        const std::string& grpc_method, bool success) {
  cluster.statsScope()
      .counter(
           fmt::format("grpc.{}.{}.{}", grpc_service, grpc_method, success ? "success" : "failure"))
      .inc();
  cluster.statsScope().counter(fmt::format("grpc.{}.{}.total", grpc_service, grpc_method)).inc();
}

Buffer::InstancePtr Common::serializeBody(const google::protobuf::Message& message) {
  // http://www.grpc.io/docs/guides/wire.html
  Buffer::InstancePtr body(new Buffer::OwnedImpl());
  uint8_t compressed = 0;
  body->add(&compressed, sizeof(compressed));
  uint32_t size = message.ByteSize();
  uint32_t network_size = htonl(size);
  body->add(&network_size, sizeof(network_size));

  BufferOutputStream buffer_stream(*body, size);
  google::protobuf::io::CodedOutputStream codec_stream(&buffer_stream);
  message.SerializeWithCachedSizes(&codec_stream);
  return body;
}

Http::MessagePtr Common::prepareHeaders(const std::string& upstream_cluster,
                                        const std::string& service_full_name,
                                        const std::string& method_name) {
  Http::MessagePtr message(new Http::RequestMessageImpl());
  message->headers().insertMethod().value(Http::Headers::get().MethodValues.Post);
  message->headers().insertPath().value().append("/", 1);
  message->headers().insertPath().value().append(service_full_name.c_str(),
                                                 service_full_name.size());
  message->headers().insertPath().value().append("/", 1);
  message->headers().insertPath().value().append(method_name.c_str(), method_name.size());
  message->headers().insertHost().value(upstream_cluster);
  message->headers().insertContentType().value(Common::GRPC_CONTENT_TYPE);

  return message;
}

void Common::checkForHeaderOnlyError(Http::Message& http_response) {
  // First check for grpc-status in headers. If it is here, we have an error.
  const Http::HeaderEntry* grpc_status_header = http_response.headers().GrpcStatus();
  if (!grpc_status_header) {
    return;
  }

  uint64_t grpc_status_code;
  if (!StringUtil::atoul(grpc_status_header->value().c_str(), grpc_status_code)) {
    throw Exception(Optional<uint64_t>(), "bad grpc-status header");
  }

  const Http::HeaderEntry* grpc_status_message = http_response.headers().GrpcMessage();
  throw Exception(grpc_status_code,
                  grpc_status_message ? grpc_status_message->value().c_str() : EMPTY_STRING);
}

void Common::validateResponse(Http::Message& http_response) {
  if (Http::Utility::getResponseStatus(http_response.headers()) != enumToInt(Http::Code::OK)) {
    throw Exception(Optional<uint64_t>(), "non-200 response code");
  }

  checkForHeaderOnlyError(http_response);

  // Check for existence of trailers.
  if (!http_response.trailers()) {
    throw Exception(Optional<uint64_t>(), "no response trailers");
  }

  const Http::HeaderEntry* grpc_status_header = http_response.trailers()->GrpcStatus();
  uint64_t grpc_status_code;
  if (!grpc_status_header ||
      !StringUtil::atoul(grpc_status_header->value().c_str(), grpc_status_code)) {
    throw Exception(Optional<uint64_t>(), "bad grpc-status trailer");
  }

  if (grpc_status_code != 0) {
    const Http::HeaderEntry* grpc_status_message = http_response.trailers()->GrpcMessage();
    throw Exception(grpc_status_code,
                    grpc_status_message ? grpc_status_message->value().c_str() : EMPTY_STRING);
  }
}

} // Grpc
