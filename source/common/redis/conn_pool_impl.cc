#include "common/redis/conn_pool_impl.h"

#include "common/common/assert.h"
#include "common/json/config_schemas.h"

namespace Redis {
namespace ConnPool {

ConfigImpl::ConfigImpl(const Json::Object& config)
    : Validator(config, Json::Schema::REDIS_CONN_POOL_SCHEMA),
      op_timeout_(config.getInteger("op_timeout_ms")) {}

ClientPtr ClientImpl::create(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                             EncoderPtr&& encoder, DecoderFactory& decoder_factory,
                             const Config& config) {

  std::unique_ptr<ClientImpl> client(
      new ClientImpl(host, dispatcher, std::move(encoder), decoder_factory, config));
  client->connection_ = host->createConnection(dispatcher).connection_;
  client->connection_->addConnectionCallbacks(*client);
  client->connection_->addReadFilter(Network::ReadFilterSharedPtr{new UpstreamReadFilter(*client)});
  client->connection_->connect();
  client->connection_->noDelay(true);
  return std::move(client);
}

ClientImpl::ClientImpl(Upstream::HostConstSharedPtr host, Event::Dispatcher& dispatcher,
                       EncoderPtr&& encoder, DecoderFactory& decoder_factory, const Config& config)
    : host_(host), encoder_(std::move(encoder)), decoder_(decoder_factory.create(*this)),
      config_(config),
      connect_or_op_timer_(dispatcher.createTimer([this]() -> void { onConnectOrOpTimeout(); })) {
  host->cluster().stats().upstream_cx_total_.inc();
  host->cluster().stats().upstream_cx_active_.inc();
  host->stats().cx_total_.inc();
  host->stats().cx_active_.inc();
  connect_or_op_timer_->enableTimer(host->cluster().connectTimeout());
}

ClientImpl::~ClientImpl() {
  ASSERT(pending_requests_.empty());
  ASSERT(connection_->state() == Network::Connection::State::Closed);
  host_->cluster().stats().upstream_cx_active_.dec();
  host_->stats().cx_active_.dec();
}

void ClientImpl::close() { connection_->close(Network::ConnectionCloseType::NoFlush); }

PoolRequest* ClientImpl::makeRequest(const RespValue& request, PoolCallbacks& callbacks) {
  ASSERT(connection_->state() == Network::Connection::State::Open);
  pending_requests_.emplace_back(*this, callbacks);
  encoder_->encode(request, encoder_buffer_);
  connection_->write(encoder_buffer_);

  // Only boost the op timeout if we are not already connected. Otherwise, we are governed by
  // the connect timeout and the timer will be reset when/if connection occurs. This allows a
  // relatively long connection spin up time for example if TLS is being used.
  if (connected_) {
    connect_or_op_timer_->enableTimer(config_.opTimeout());
  }

  return &pending_requests_.back();
}

void ClientImpl::onConnectOrOpTimeout() {
  if (connected_) {
    host_->cluster().stats().upstream_rq_timeout_.inc();
  } else {
    host_->cluster().stats().upstream_cx_connect_timeout_.inc();
  }
  connection_->close(Network::ConnectionCloseType::NoFlush);
}

void ClientImpl::onData(Buffer::Instance& data) {
  try {
    decoder_->decode(data);
  } catch (ProtocolError&) {
    host_->cluster().stats().upstream_cx_protocol_error_.inc();
    connection_->close(Network::ConnectionCloseType::NoFlush);
  }
}

void ClientImpl::onEvent(uint32_t events) {
  if ((events & Network::ConnectionEvent::RemoteClose) ||
      (events & Network::ConnectionEvent::LocalClose)) {
    if (!pending_requests_.empty()) {
      host_->cluster().stats().upstream_cx_destroy_with_active_rq_.inc();
      if (events & Network::ConnectionEvent::RemoteClose) {
        host_->cluster().stats().upstream_cx_destroy_remote_with_active_rq_.inc();
      }
      if (events & Network::ConnectionEvent::LocalClose) {
        host_->cluster().stats().upstream_cx_destroy_local_with_active_rq_.inc();
      }
    }

    while (!pending_requests_.empty()) {
      PendingRequest& request = pending_requests_.front();
      if (!request.canceled_) {
        request.callbacks_.onFailure();
      } else {
        host_->cluster().stats().upstream_rq_cancelled_.inc();
      }
      pending_requests_.pop_front();
    }

    connect_or_op_timer_->disableTimer();
  } else if (events & Network::ConnectionEvent::Connected) {
    connected_ = true;
    ASSERT(!pending_requests_.empty());
    connect_or_op_timer_->enableTimer(config_.opTimeout());
  }

  if ((events & Network::ConnectionEvent::RemoteClose) && !connected_) {
    host_->cluster().stats().upstream_cx_connect_fail_.inc();
    host_->stats().cx_connect_fail_.inc();
  }
}

void ClientImpl::onRespValue(RespValuePtr&& value) {
  ASSERT(!pending_requests_.empty());
  PendingRequest& request = pending_requests_.front();
  if (!request.canceled_) {
    request.callbacks_.onResponse(std::move(value));
  } else {
    host_->cluster().stats().upstream_rq_cancelled_.inc();
  }
  pending_requests_.pop_front();

  // We boost the op timeout every time we pipeline a new op. However, if there are no remaining
  // ops in the pipeline we need to disable the timer.
  if (pending_requests_.empty()) {
    connect_or_op_timer_->disableTimer();
  }
}

ClientImpl::PendingRequest::PendingRequest(ClientImpl& parent, PoolCallbacks& callbacks)
    : parent_(parent), callbacks_(callbacks) {
  parent.host_->cluster().stats().upstream_rq_total_.inc();
  parent.host_->cluster().stats().upstream_rq_active_.inc();
  parent.host_->stats().rq_total_.inc();
  parent.host_->stats().rq_active_.inc();
}

ClientImpl::PendingRequest::~PendingRequest() {
  parent_.host_->cluster().stats().upstream_rq_active_.dec();
  parent_.host_->stats().rq_active_.dec();
}

void ClientImpl::PendingRequest::cancel() {
  // If we get a cancellation, we just mark the pending request as cancelled, and then we drop
  // the response as it comes through. There is no reason to blow away the connection when the
  // remote is already responding as fast as possible.
  canceled_ = true;
}

ClientFactoryImpl ClientFactoryImpl::instance_;

ClientPtr ClientFactoryImpl::create(Upstream::HostConstSharedPtr host,
                                    Event::Dispatcher& dispatcher, const Config& config) {
  return ClientImpl::create(host, dispatcher, EncoderPtr{new EncoderImpl()}, decoder_factory_,
                            config);
}

InstanceImpl::InstanceImpl(const std::string& cluster_name, Upstream::ClusterManager& cm,
                           ClientFactory& client_factory, ThreadLocal::Instance& tls,
                           const Json::Object& config)
    : cm_(cm), client_factory_(client_factory), tls_(tls), tls_slot_(tls.allocateSlot()),
      config_(config) {
  tls.set(tls_slot_, [this, cluster_name](
                         Event::Dispatcher& dispatcher) -> ThreadLocal::ThreadLocalObjectSharedPtr {
    return std::make_shared<ThreadLocalPool>(*this, dispatcher, cluster_name);
  });
}

PoolRequest* InstanceImpl::makeRequest(const std::string& hash_key, const RespValue& value,
                                       PoolCallbacks& callbacks) {
  return tls_.getTyped<ThreadLocalPool>(tls_slot_).makeRequest(hash_key, value, callbacks);
}

InstanceImpl::ThreadLocalPool::ThreadLocalPool(InstanceImpl& parent, Event::Dispatcher& dispatcher,
                                               const std::string& cluster_name)
    : parent_(parent), dispatcher_(dispatcher), cluster_(parent_.cm_.get(cluster_name)) {

  cluster_->hostSet().addMemberUpdateCb(
      [this](const std::vector<Upstream::HostSharedPtr>&,
             const std::vector<Upstream::HostSharedPtr>& hosts_removed)
          -> void { onHostsRemoved(hosts_removed); });
}

void InstanceImpl::ThreadLocalPool::onHostsRemoved(
    const std::vector<Upstream::HostSharedPtr>& hosts_removed) {
  for (auto host : hosts_removed) {
    auto it = client_map_.find(host);
    if (it != client_map_.end()) {
      // We don't currently support any type of draining for redis connections. If a host is gone,
      // we just close the connection. This will fail any pending requests.
      it->second->redis_client_->close();
    }
  }
}

PoolRequest* InstanceImpl::ThreadLocalPool::makeRequest(const std::string& hash_key,
                                                        const RespValue& request,
                                                        PoolCallbacks& callbacks) {
  LbContextImpl lb_context(hash_key);
  Upstream::HostConstSharedPtr host = cluster_->loadBalancer().chooseHost(&lb_context);
  if (!host) {
    return nullptr;
  }

  ThreadLocalActiveClientPtr& client = client_map_[host];
  if (!client) {
    client.reset(new ThreadLocalActiveClient(*this));
    client->host_ = host;
    client->redis_client_ = parent_.client_factory_.create(host, dispatcher_, parent_.config_);
    client->redis_client_->addConnectionCallbacks(*client);
  }

  return client->redis_client_->makeRequest(request, callbacks);
}

void InstanceImpl::ThreadLocalActiveClient::onEvent(uint32_t events) {
  if ((events & Network::ConnectionEvent::RemoteClose) ||
      (events & Network::ConnectionEvent::LocalClose)) {
    auto client_to_delete = parent_.client_map_.find(host_);
    ASSERT(client_to_delete != parent_.client_map_.end());
    parent_.dispatcher_.deferredDelete(std::move(client_to_delete->second));
    parent_.client_map_.erase(client_to_delete);
  }
}

void InstanceImpl::ThreadLocalPool::shutdown() {
  while (!client_map_.empty()) {
    client_map_.begin()->second->redis_client_->close();
  }
}

} // ConnPool
} // Redis
