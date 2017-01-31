#include "common.h"
#include "rpc_channel_impl.h"

#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"

#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/message.h"

namespace Grpc {

class BufferInputStream : public google::protobuf::io::ZeroCopyInputStream {
public:
  BufferInputStream(const Buffer::Instance& buffer) {
    num_iovecs_ = buffer.getRawSlices(nullptr, 0);
    iovecs_.resize(num_iovecs_);
    buffer.getRawSlices(&iovecs_[0], num_iovecs_);
  }

  // google::protobuf::io::ZeroCopyInputStream
  bool Next(const void** data , int* size) override {
    current_iovec_++;
    ASSERT(current_iovec_ <= num_iovecs_);
    *data = iovecs_[current_iovec_].mem_;
    *size = iovecs_[current_iovec_].len_;
    read_ += *size;
    return true;
  }

  void BackUp(int count) override {
    ASSERT(current_iovec_ >= 0);
    ASSERT(iovecs_[current_iovec_].len_ >= static_cast<uint64_t>(count));
    ASSERT(read_ >= static_cast<uint64_t>(count));
    iovecs_[current_iovec_].len_ -= count;
    read_ -= count;
  }

  int64_t ByteCount() const override {
    return read_;
  }

  bool Skip(int count) override {
  }

private:
  std::vector<Buffer::RawSlice> iovecs_;
  int64_t num_iovecs_;
  int64_t current_iovec_{-1};
  uint64_t read_{};
};

void RpcChannelImpl::cancel() {
  http_request_->cancel();
  onComplete();
}

void RpcChannelImpl::CallMethod(const proto::MethodDescriptor* method, proto::RpcController*,
                                const proto::Message* grpc_request, proto::Message* grpc_response,
                                proto::Closure*) {
  ASSERT(!http_request_ && !grpc_method_ && !grpc_response_);
  grpc_method_ = method;
  grpc_response_ = grpc_response;

  // For proto3 messages this should always return true.
  ASSERT(grpc_request->IsInitialized());

  // This should be caught in configuration, and a request will fail normally anyway, but assert
  // here for clarity.
  ASSERT(cluster_->features() & Upstream::ClusterInfo::Features::HTTP2);

  Http::MessagePtr message =
      Common::prepareHeaders(cluster_->name(), method->service()->full_name(), method->name());
  message->body(Common::serializeBody(*grpc_request));

  callbacks_.onPreRequestCustomizeHeaders(message->headers());
  http_request_ =
      cm_.httpAsyncClientForCluster(cluster_->name()).send(std::move(message), *this, timeout_);
}

void RpcChannelImpl::incStat(bool success) {
  Common::chargeStat(*cluster_, grpc_method_->service()->full_name(), grpc_method_->name(),
                     success);
}

void RpcChannelImpl::onSuccess(Http::MessagePtr&& http_response) {
  try {
    Common::validateResponse(*http_response);

    // A gRPC response contains a 5 byte header. Currently we only support unary responses so we
    // ignore the header. @see serializeBody().
    if (!http_response->body() || !(http_response->body()->length() > 5)) {
      throw Exception(Optional<uint64_t>(), "bad serialized body");
    }

    http_response->body()->drain(5);
    BufferInputStream input_stream(*http_response->body());
    if (!grpc_response_->ParseFromBoundedZeroCopyStream(&input_stream,
                                                        http_response->body()->length())) {
      throw Exception(Optional<uint64_t>(), "bad serialized body");
    }

    callbacks_.onSuccess();
    incStat(true);
    onComplete();
  } catch (const Exception& e) {
    onFailureWorker(e.grpc_status_, e.what());
  }
}

void RpcChannelImpl::onFailureWorker(const Optional<uint64_t>& grpc_status,
                                     const std::string& message) {
  callbacks_.onFailure(grpc_status, message);
  incStat(false);
  onComplete();
}

void RpcChannelImpl::onFailure(Http::AsyncClient::FailureReason reason) {
  switch (reason) {
  case Http::AsyncClient::FailureReason::Reset:
    onFailureWorker(Optional<uint64_t>(), "stream reset");
    break;
  }
}

void RpcChannelImpl::onComplete() {
  http_request_ = nullptr;
  grpc_method_ = nullptr;
  grpc_response_ = nullptr;
}

} // Grpc
