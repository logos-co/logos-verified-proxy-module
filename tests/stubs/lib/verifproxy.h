/*
 * Stub of nimbus-eth1's nimbus_verified_proxy/library/verifproxy.h.
 *
 * Unit tests link mocks/mock_libverifproxy.cpp instead of the real ~100 MB
 * archive (flake.nix sets tests.mockCLibs = ["verifproxy"], which keeps the
 * upstream build out of the test derivation entirely). Only the declarations
 * the module actually uses are reproduced; keep the signatures byte-identical
 * to upstream or the mock will not match the real thing.
 */
#ifndef VERIFPROXY_STUB_H
#define VERIFPROXY_STUB_H

#include <stdbool.h>
#include <stddef.h>

#define RET_SUCCESS      0
#define RET_ERROR       -1
#define RET_CANCELLED   -2
#define RET_DESER_ERROR -3

#ifdef __cplusplus
extern "C" {
#endif

void NimMain(void);

typedef struct Context Context;

typedef void (*CallBackProc)(Context *ctx, int status, char *result, void *userData);
typedef void (*TransportDeliveryCallback)(int status, char *res, void *userData);
typedef void (*ExecutionTransportProc)(Context *ctx, TransportDeliveryCallback cb, void *userData);
typedef void (*BeaconTransportProc)(Context *ctx, TransportDeliveryCallback cb, void *userData);

Context *startVerifProxy(char *configJson,
                         ExecutionTransportProc executionTransport,
                         BeaconTransportProc beaconTransport);
void     stopVerifProxy(Context *ctx);
void     freeContext(Context *ctx);
int      processVerifProxyTasks(Context *ctx);
void     proxyCall(Context *ctx, char *name, char *params, CallBackProc cb, void *userData);
void     freeNimAllocatedString(char *res);

#ifdef __cplusplus
}
#endif

#endif /* VERIFPROXY_STUB_H */
