// Test bodies for VerifiedProxyImpl's `logos_events:` methods.
//
// Production builds get these from the codegen-emitted
// `verified_proxy_module_events_cdylib.cpp`, which marshals through the host;
// unit tests construct the impl directly without that layer, so they link
// these one-line forwarders instead. Observe with logos_test::EventCapture.

#include <logos_test.h>
#include "verified_proxy_impl.h"

using logos_test::recordEvent;

void VerifiedProxyImpl::proxyStarted(const std::string& payload)      { recordEvent("proxyStarted", payload); }
void VerifiedProxyImpl::proxyStopped(const std::string& payload)      { recordEvent("proxyStopped", payload); }
void VerifiedProxyImpl::proxyStateChanged(const std::string& payload) { recordEvent("proxyStateChanged", payload); }
