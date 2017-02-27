#pragma once

namespace Upstream {

/**
 * A thread local cluster instance that can be used for direct load balancing and host set
 * interactions.
 */
class ThreadLocalCluster {
public:
  virtual ~ThreadLocalCluster() {}

  /**
   * @return const HostSet& the backing host set.
   */
  virtual const HostSet& hostSet() PURE;

  /**
   * @return ClusterInfoPtr fixfix.
   */
  virtual ClusterInfoPtr info() PURE;

  /**
   * @return LoadBalancer& the backing load balancer.
   */
  virtual LoadBalancer& loadBalancer() PURE;
};

} // Upstream
