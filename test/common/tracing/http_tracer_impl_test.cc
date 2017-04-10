#include "common/common/base64.h"
#include "common/http/headers.h"
#include "common/http/header_map_impl.h"
#include "common/http/message_impl.h"
#include "common/runtime/runtime_impl.h"
#include "common/runtime/uuid_util.h"
#include "common/tracing/http_tracer_impl.h"
#include "common/tracing/lightstep_tracer_impl.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::Test;

namespace Tracing {

TEST(HttpTracerUtilityTest, mutateHeaders) {
  // Sampling, global on.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.random_sampling", 0, _, 10000))
        .WillOnce(Return(true));
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::Sampled,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Sampling must not be done on client traced.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.random_sampling", 0, _, 10000)).Times(0);
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {"x-request-id", "125a4afb-6f55-a4ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::Forced,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Sampling, global off.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.random_sampling", 0, _, 10000))
        .WillOnce(Return(true));
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(false));

    Http::TestHeaderMapImpl request_headers{
        {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::NoTrace,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Client, client enabled, global on.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.client_enabled", 100))
        .WillOnce(Return(true));
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {"x-client-trace-id", "f4dca0a9-12c7-4307-8002-969403baf480"},
        {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::Client,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Client, client disabled, global on.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.client_enabled", 100))
        .WillOnce(Return(false));
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {"x-client-trace-id", "f4dca0a9-12c7-4307-8002-969403baf480"},
        {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::NoTrace,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Forced, global on.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(true));

    Http::TestHeaderMapImpl request_headers{
        {"x-envoy-force-trace", "true"}, {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::Forced,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Forced, global off.
  {
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("tracing.global_enabled", 100, _))
        .WillOnce(Return(false));

    Http::TestHeaderMapImpl request_headers{
        {"x-envoy-force-trace", "true"}, {"x-request-id", "125a4afb-6f55-44ba-ad80-413f09f48a28"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::NoTrace,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }

  // Forced, global on, broken uuid.
  {
    NiceMock<Runtime::MockLoader> runtime;

    Http::TestHeaderMapImpl request_headers{{"x-envoy-force-trace", "true"},
                                            {"x-request-id", "bb"}};
    HttpTracerUtility::mutateHeaders(request_headers, runtime);

    EXPECT_EQ(UuidTraceStatus::NoTrace,
              UuidUtils::isTraceableUuid(request_headers.get_("x-request-id")));
  }
}

TEST(HttpTracerUtilityTest, IsTracing) {
  NiceMock<Http::AccessLog::MockRequestInfo> request_info;
  NiceMock<Stats::MockStore> stats;
  Runtime::RandomGeneratorImpl random;
  std::string not_traceable_guid = random.uuid();

  std::string forced_guid = random.uuid();
  UuidUtils::setTraceableUuid(forced_guid, UuidTraceStatus::Forced);
  Http::TestHeaderMapImpl forced_header{{"x-request-id", forced_guid}};

  std::string sampled_guid = random.uuid();
  UuidUtils::setTraceableUuid(sampled_guid, UuidTraceStatus::Sampled);
  Http::TestHeaderMapImpl sampled_header{{"x-request-id", sampled_guid}};

  std::string client_guid = random.uuid();
  UuidUtils::setTraceableUuid(client_guid, UuidTraceStatus::Client);
  Http::TestHeaderMapImpl client_header{{"x-request-id", client_guid}};

  Http::TestHeaderMapImpl not_traceable_header{{"x-request-id", not_traceable_guid}};
  Http::TestHeaderMapImpl empty_header{};

  // Force traced.
  {
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(false));

    Decision result = HttpTracerUtility::isTracing(request_info, forced_header);
    EXPECT_EQ(Reason::ServiceForced, result.reason);
    EXPECT_TRUE(result.is_tracing);
  }

  // Sample traced.
  {
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(false));

    Decision result = HttpTracerUtility::isTracing(request_info, sampled_header);
    EXPECT_EQ(Reason::Sampling, result.reason);
    EXPECT_TRUE(result.is_tracing);
  }

  // HC request.
  {
    Http::TestHeaderMapImpl traceable_header_hc{{"x-request-id", forced_guid}};
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(true));

    Decision result = HttpTracerUtility::isTracing(request_info, traceable_header_hc);
    EXPECT_EQ(Reason::HealthCheck, result.reason);
    EXPECT_FALSE(result.is_tracing);
  }

  // Client traced.
  {
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(false));

    Decision result = HttpTracerUtility::isTracing(request_info, client_header);
    EXPECT_EQ(Reason::ClientForced, result.reason);
    EXPECT_TRUE(result.is_tracing);
  }

  // No request id.
  {
    Http::TestHeaderMapImpl headers;
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(false));
    Decision result = HttpTracerUtility::isTracing(request_info, headers);
    EXPECT_EQ(Reason::NotTraceableRequestId, result.reason);
    EXPECT_FALSE(result.is_tracing);
  }

  // Broken request id.
  {
    Http::TestHeaderMapImpl headers{{"x-request-id", "not-real-x-request-id"}};
    EXPECT_CALL(request_info, healthCheck()).WillOnce(Return(false));
    Decision result = HttpTracerUtility::isTracing(request_info, headers);
    EXPECT_EQ(Reason::NotTraceableRequestId, result.reason);
    EXPECT_FALSE(result.is_tracing);
  }
}

TEST(HttpTracerUtilityTest, OriginalAndLongPath) {
  const std::string path(300, 'a');
  const std::string expected_path(128, 'a');
  std::unique_ptr<NiceMock<MockSpan>> span(new NiceMock<MockSpan>());

  Http::TestHeaderMapImpl request_headers{
      {"x-request-id", "id"}, {"x-envoy-original-path", path}, {":method", "GET"}};
  NiceMock<Http::AccessLog::MockRequestInfo> request_info;

  Http::Protocol protocol = Http::Protocol::Http2;
  EXPECT_CALL(request_info, bytesReceived()).WillOnce(Return(10));
  EXPECT_CALL(request_info, bytesSent()).WillOnce(Return(11));
  EXPECT_CALL(request_info, protocol()).WillOnce(Return(protocol));
  Optional<uint32_t> response_code;
  EXPECT_CALL(request_info, responseCode()).WillRepeatedly(ReturnRef(response_code));

  EXPECT_CALL(*span, setTag(_, _)).Times(testing::AnyNumber());
  EXPECT_CALL(*span, setTag("request_line", "GET " + expected_path + " HTTP/2"));
  NiceMock<MockConfig> config;
  HttpTracerUtility::finalizeSpan(*span, request_headers, request_info, config);
}

TEST(HttpTracerUtilityTest, SpanOptionalHeaders) {
  std::unique_ptr<NiceMock<MockSpan>> span(new NiceMock<MockSpan>());

  Http::TestHeaderMapImpl request_headers{
      {"x-request-id", "id"}, {":path", "/test"}, {":method", "GET"}};
  NiceMock<Http::AccessLog::MockRequestInfo> request_info;

  Http::Protocol protocol = Http::Protocol::Http10;
  EXPECT_CALL(request_info, bytesReceived()).WillOnce(Return(10));
  EXPECT_CALL(request_info, protocol()).WillOnce(Return(protocol));
  const std::string service_node = "i-453";

  // Check that span is populated correctly.
  EXPECT_CALL(*span, setTag("guid:x-request-id", "id"));
  EXPECT_CALL(*span, setTag("request_line", "GET /test HTTP/1.0"));
  EXPECT_CALL(*span, setTag("host_header", "-"));
  EXPECT_CALL(*span, setTag("user_agent", "-"));
  EXPECT_CALL(*span, setTag("downstream_cluster", "-"));
  EXPECT_CALL(*span, setTag("request_size", "10"));

  Optional<uint32_t> response_code;
  EXPECT_CALL(request_info, responseCode()).WillRepeatedly(ReturnRef(response_code));
  EXPECT_CALL(request_info, bytesSent()).WillOnce(Return(100));

  EXPECT_CALL(*span, setTag("response_code", "0"));
  EXPECT_CALL(*span, setTag("response_size", "100"));
  EXPECT_CALL(*span, setTag("response_flags", "-"));

  EXPECT_CALL(*span, finishSpan());
  NiceMock<MockConfig> config;
  HttpTracerUtility::finalizeSpan(*span, request_headers, request_info, config);
}

TEST(HttpTracerUtilityTest, SpanPopulatedFailureResponse) {
  std::unique_ptr<NiceMock<MockSpan>> span(new NiceMock<MockSpan>());
  Http::TestHeaderMapImpl request_headers{
      {"x-request-id", "id"}, {":path", "/test"}, {":method", "GET"}};
  NiceMock<Http::AccessLog::MockRequestInfo> request_info;
  Http::Protocol protocol = Http::Protocol::Http10;
  EXPECT_CALL(request_info, protocol()).WillOnce(Return(protocol));

  request_headers.insertHost().value(std::string("api"));
  request_headers.insertUserAgent().value(std::string("agent"));
  request_headers.insertEnvoyDownstreamServiceCluster().value(std::string("downstream_cluster"));
  request_headers.insertClientTraceId().value(std::string("client_trace_id"));

  EXPECT_CALL(request_info, bytesReceived()).WillOnce(Return(10));
  const std::string service_node = "i-453";

  // Check that span is populated correctly.
  EXPECT_CALL(*span, setTag("guid:x-request-id", "id"));
  EXPECT_CALL(*span, setTag("request_line", "GET /test HTTP/1.0"));
  EXPECT_CALL(*span, setTag("host_header", "api"));
  EXPECT_CALL(*span, setTag("user_agent", "agent"));
  EXPECT_CALL(*span, setTag("downstream_cluster", "downstream_cluster"));
  EXPECT_CALL(*span, setTag("request_size", "10"));
  EXPECT_CALL(*span, setTag("guid:x-client-trace-id", "client_trace_id"));

  // Check that span has tags from custom headers.
  request_headers.addViaCopy(Http::LowerCaseString("aa"), "a");
  request_headers.addViaCopy(Http::LowerCaseString("bb"), "b");
  request_headers.addViaCopy(Http::LowerCaseString("cc"), "c");
  MockConfig config;
  config.headers_.push_back(Http::LowerCaseString("aa"));
  config.headers_.push_back(Http::LowerCaseString("cc"));
  config.headers_.push_back(Http::LowerCaseString("ee"));
  EXPECT_CALL(*span, setTag("aa", "a"));
  EXPECT_CALL(*span, setTag("cc", "c"));
  EXPECT_CALL(config, requestHeadersForTags());

  Optional<uint32_t> response_code(503);
  EXPECT_CALL(request_info, responseCode()).WillRepeatedly(ReturnRef(response_code));
  EXPECT_CALL(request_info, bytesSent()).WillOnce(Return(100));
  ON_CALL(request_info, getResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout))
      .WillByDefault(Return(true));

  EXPECT_CALL(*span, setTag("error", "true"));
  EXPECT_CALL(*span, setTag("response_code", "503"));
  EXPECT_CALL(*span, setTag("response_size", "100"));
  EXPECT_CALL(*span, setTag("response_flags", "UT"));

  EXPECT_CALL(*span, finishSpan());
  HttpTracerUtility::finalizeSpan(*span, request_headers, request_info, config);
}

TEST(HttpTracerUtilityTest, operationTypeToString) {
  EXPECT_EQ("ingress", HttpTracerUtility::toString(OperationName::Ingress));
  EXPECT_EQ("egress", HttpTracerUtility::toString(OperationName::Egress));
}

TEST(HttpNullTracerTest, BasicFunctionality) {
  HttpNullTracer null_tracer;
  MockConfig config;
  Http::AccessLog::MockRequestInfo request_info;
  Http::TestHeaderMapImpl request_headers;

  EXPECT_EQ(nullptr, null_tracer.startSpan(config, request_headers, request_info));
}

class HttpTracerImplTest : public Test {
public:
  HttpTracerImplTest() {
    driver_ = new MockDriver();
    DriverPtr driver_ptr(driver_);
    tracer_.reset(new HttpTracerImpl(std::move(driver_ptr), local_info_));
  }

  Http::TestHeaderMapImpl request_headers_{
      {":path", "/"}, {":method", "GET"}, {"x-request-id", "foo"}, {":authority", "test"}};
  Http::AccessLog::MockRequestInfo request_info_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  MockConfig config_;
  MockDriver* driver_;
  HttpTracerPtr tracer_;
};

TEST_F(HttpTracerImplTest, BasicFunctionalityNullSpan) {
  EXPECT_CALL(config_, operationName()).Times(2);
  EXPECT_CALL(request_info_, startTime());
  const std::string operation_name = "ingress";
  EXPECT_CALL(*driver_, startSpan_(_, operation_name, request_info_.start_time_))
      .WillOnce(Return(nullptr));

  tracer_->startSpan(config_, request_headers_, request_info_);
}

TEST_F(HttpTracerImplTest, BasicFunctionalityNodeSet) {
  EXPECT_CALL(request_info_, startTime());
  EXPECT_CALL(local_info_, nodeName());
  EXPECT_CALL(config_, operationName()).Times(2).WillRepeatedly(Return(OperationName::Egress));

  NiceMock<MockSpan>* span = new NiceMock<MockSpan>();
  const std::string operation_name = "egress test";
  EXPECT_CALL(*driver_, startSpan_(_, operation_name, request_info_.start_time_))
      .WillOnce(Return(span));

  EXPECT_CALL(*span, setTag(_, _)).Times(testing::AnyNumber());
  EXPECT_CALL(*span, setTag("node_id", "node_name"));

  tracer_->startSpan(config_, request_headers_, request_info_);
}

} // Tracing
