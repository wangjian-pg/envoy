#include "source/common/router/vhds.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "envoy/config/core/v3/config_source.pb.h"
#include "envoy/config/subscription.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/fmt.h"
#include "source/common/config/api_version.h"
#include "source/common/config/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/common/router/config_impl.h"

namespace Envoy {
namespace Router {

// Implements callbacks to handle DeltaDiscovery protocol for VirtualHostDiscoveryService
VhdsSubscription::VhdsSubscription(RouteConfigUpdatePtr& config_update_info,
                                   Server::Configuration::ServerFactoryContext& factory_context,
                                   const std::string& stat_prefix,
                                   Rds::RouteConfigProvider* route_config_provider)
    : Envoy::Config::SubscriptionBase<envoy::config::route::v3::VirtualHost>(
          factory_context.messageValidationContext().dynamicValidationVisitor(), "name"),
      config_update_info_(config_update_info),
      scope_(factory_context.scope().createScope(
          stat_prefix + "vhds." + config_update_info_->protobufConfigurationCast().name() + ".")),
      stats_({ALL_VHDS_STATS(POOL_COUNTER(*scope_))}),
      route_config_provider_(route_config_provider){
  // TODO(wangjian.pg 20230706), makes VHDS works on ADS stream.
  const auto& config_source = config_update_info_->protobufConfigurationCast()
                                  .vhds()
                                  .config_source()
                                  .api_config_source()
                                  .api_type();
  if (config_source != envoy::config::core::v3::ApiConfigSource::DELTA_GRPC) {
    //throw EnvoyException("vhds: only 'DELTA_GRPC' is supported as an api_type.");
  }
  const auto resource_name = getResourceName();
  //Envoy::Config::SubscriptionOptions options;
  //options.use_namespace_matching_ = true;
  subscription_ =
      factory_context.clusterManager().subscriptionFactory().subscriptionFromConfigSource(
          config_update_info_->protobufConfigurationCast().vhds().config_source(),
          Grpc::Common::typeUrl(resource_name), *scope_,
          *this, resource_decoder_, {});
}

void VhdsSubscription::updateOnDemand(const std::string& with_route_config_name_prefix) {
  // TODO(wangjian.pg 20230710) if domain names already exits, we do not need to updateResourceInterest.
  on_demand_domains_.insert(with_route_config_name_prefix);
  if (!started_){
    subscription_->start({with_route_config_name_prefix});
    // TODO(wangjian.pg 20230710) insert or emplace
    started_ = true;
    return;
  }
  subscription_->updateResourceInterest(on_demand_domains_);
  // TODO, we may not need the requestOnDemandUpdate interface
  // keep all the resource names we are interestd in and just call the method
  // subscription_->updateResourceInterested.
  // subscription_->requestOnDemandUpdate({with_route_config_name_prefix});
}

void VhdsSubscription::onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason reason,
                                            const EnvoyException*) {
  ASSERT(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure != reason);
  // We need to allow server startup to continue, even if we have a bad
  // config.
  // init_target_.ready();
  // TODO(wangjian.pg) what would happend if on-demand request failed because of timeout or others reasons?
  // FIXME(wangjian.pg 20230707), May trigger all pending callbacks to fail?
}

void VhdsSubscription::onConfigUpdate(
    const std::vector<Envoy::Config::DecodedResourceRef>& added_resources,
    const Protobuf::RepeatedPtrField<std::string>& removed_resources,
    const std::string& version_info) {
  RouteConfigUpdateReceiver::VirtualHostRefVector added_vhosts;
  std::set<std::string> added_resource_ids;
  for (const auto& resource : added_resources) {
    added_resource_ids.emplace(resource.get().name());
    std::copy(resource.get().aliases().begin(), resource.get().aliases().end(),
              std::inserter(added_resource_ids, added_resource_ids.end()));
    // the management server returns empty resources (they contain no virtual hosts in this case)
    // for aliases that it couldn't resolve.
    if (!resource.get().hasResource()) {
      continue;
    }
    added_vhosts.emplace_back(
        dynamic_cast<const envoy::config::route::v3::VirtualHost&>(resource.get().resource()));
  }
  if (config_update_info_->onVhdsUpdate(added_vhosts, added_resource_ids, removed_resources,
                                        version_info)) {
    stats_.config_reload_.inc();
    ENVOY_LOG(debug, "vhds: loading new configuration: config_name={} hash={}",
              config_update_info_->protobufConfigurationCast().name(),
              config_update_info_->configHash());
    if (route_config_provider_ != nullptr) {
      route_config_provider_->onConfigUpdate();
    }
  }

  //init_target_.ready();
}

} // namespace Router
} // namespace Envoy
