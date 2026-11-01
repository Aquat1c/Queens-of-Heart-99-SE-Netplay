#pragma once

#if !defined(QOH99_NODE_RUNTIME_TEST_SEAM)
#error "NodeRuntimeTestSeams.h is test-only"
#endif

#include "../ResilientSpectatorNodeRuntime.h"

namespace qclient::spectator::resilient::test {

struct QtaAuthorityContextTestFactory {
    static std::shared_ptr<QtaAuthorityContext> make(
        const uint64_t authorityGeneration,
        const uint64_t localExpiryDeadlineMs,
        const size_t maximumRedemptions = 16u) {
        return QtaAuthorityContext::create(authorityGeneration,
                                           localExpiryDeadlineMs,
                                           maximumRedemptions);
    }

    static void revoke(const std::shared_ptr<QtaAuthorityContext>& context) {
        if (context != nullptr) context->revoke();
    }
};

struct QtaVerifiedParentLeaseTestFactory {
    static QtaVerifiedParentLease make(
        qnetplay::spectator::resilient::ParentLease lease,
        std::shared_ptr<QtaAuthorityContext> authority) {
        return QtaVerifiedParentLease(std::move(lease), std::move(authority));
    }
};

struct QtaVerifiedCanonicalIngressTestFactory {
    static QtaVerifiedCanonicalIngress make(
        std::shared_ptr<QtaAuthorityContext> authority,
        const ResilientSpectatorNodeRuntimeConfig& config,
        const qnetplay::spectator::SpectatorViewer& viewer) {
        return QtaVerifiedCanonicalIngress(std::move(authority), config,
                                           viewer);
    }
};

struct QtaVerifiedChildRetirementTestFactory {
    static NodeRuntimeResult retire(
        ResilientSpectatorNodeRuntime& runtime,
        QtaVerifiedChildRetirement&& retirement, const uint64_t nowMs) {
        return runtime.retireQtaVerifiedChild(std::move(retirement), nowMs);
    }
};

struct NodeRuntimeChildIngressTestFactory {
    static NodeRuntimeChildIngress make(
        const qnetplay::spectator::resilient::RouteDescriptor& observedQspRoute,
        const uint64_t endpointBindingGeneration,
        const qnetplay::spectator::EndpointId endpoint) {
        return NodeRuntimeChildIngress(observedQspRoute,
                                       endpointBindingGeneration, endpoint);
    }
};

}
