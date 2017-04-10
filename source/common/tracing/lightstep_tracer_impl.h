#pragma once

#include "envoy/runtime/runtime.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/tracing/http_tracer.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/http/header_map_impl.h"
#include "common/http/message_impl.h"
#include "common/json/json_loader.h"

#include "lightstep/carrier.h"
#include "lightstep/tracer.h"

namespace Tracing {

#define LIGHTSTEP_TRACER_STATS(COUNTER)                                                            \
  COUNTER(spans_sent)                                                                              \
  COUNTER(timer_flushed)

struct LightstepTracerStats {
  LIGHTSTEP_TRACER_STATS(GENERATE_COUNTER_STRUCT)
};

class LightStepSpan : public Span {
public:
  LightStepSpan(lightstep::Span& span);

  // Tracing::Span
  void finishSpan() override;
  void setTag(const std::string& name, const std::string& value) override;

  lightstep::SpanContext context() { return span_.context(); }

private:
  lightstep::Span span_;
};

typedef std::unique_ptr<LightStepSpan> LightStepSpanPtr;

/**
 * LightStep (http://lightstep.com/) provides tracing capabilities, aggregation, visualization of
 * application trace data.
 *
 * LightStepSink is for flushing data to LightStep collectors.
 */
class LightStepDriver : public Driver {
public:
  LightStepDriver(const Json::Object& config, Upstream::ClusterManager& cluster_manager,
                  Stats::Store& stats, ThreadLocal::Instance& tls, Runtime::Loader& runtime,
                  std::unique_ptr<lightstep::TracerOptions> options);

  // Tracer::TracingDriver
  SpanPtr startSpan(Http::HeaderMap& request_headers, const std::string& operation_name,
                    SystemTime start_time) override;

  Upstream::ClusterManager& clusterManager() { return cm_; }
  Upstream::ClusterInfoConstSharedPtr cluster() { return cluster_; }
  Runtime::Loader& runtime() { return runtime_; }
  LightstepTracerStats& tracerStats() { return tracer_stats_; }

private:
  struct TlsLightStepTracer : ThreadLocal::ThreadLocalObject {
    TlsLightStepTracer(lightstep::Tracer tracer, LightStepDriver& driver);

    void shutdown() override {}

    lightstep::Tracer tracer_;
    LightStepDriver& driver_;
  };

  Upstream::ClusterManager& cm_;
  Upstream::ClusterInfoConstSharedPtr cluster_;
  LightstepTracerStats tracer_stats_;
  ThreadLocal::Instance& tls_;
  Runtime::Loader& runtime_;
  std::unique_ptr<lightstep::TracerOptions> options_;
  uint32_t tls_slot_;
};

class LightStepRecorder : public lightstep::Recorder, Http::AsyncClient::Callbacks {
public:
  LightStepRecorder(const lightstep::TracerImpl& tracer, LightStepDriver& driver,
                    Event::Dispatcher& dispatcher);

  // lightstep::Recorder
  void RecordSpan(lightstep::collector::Span&& span) override;
  bool FlushWithTimeout(lightstep::Duration) override;

  // Http::AsyncClient::Callbacks
  void onSuccess(Http::MessagePtr&&) override;
  void onFailure(Http::AsyncClient::FailureReason) override;

  static std::unique_ptr<lightstep::Recorder> NewInstance(LightStepDriver& driver,
                                                          Event::Dispatcher& dispatcher,
                                                          const lightstep::TracerImpl& tracer);

private:
  void enableTimer();
  void flushSpans();

  lightstep::ReportBuilder builder_;
  LightStepDriver& driver_;
  Event::TimerPtr flush_timer_;
};

} // Tracing
