// The typed eth_*/op_* surface.
//
// Every one of these is three lines over the SAME dispatch path: the library's
// proxyCall is a string `case` over the very procs its typed C entry points
// call, so there is one FFI path here rather than sixty-one. They exist for
// discoverability — `lm methods`, the LIDL contract, and a caller's type
// checker — not because each needs its own binding.
//
// Generated. See tools/gen_rpc_methods.py; the table comes from the library's
// own dispatch table, so a bumped nimbus input cannot silently leave this
// behind.

#include "verified_proxy_impl.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// BEGIN GENERATED RPC WRAPPERS -- edit tools/gen_rpc_methods.py, not this
StdLogosResult VerifiedProxyImpl::ethChainId() {
    return rpc("eth_chainId", json::array({}));
}

StdLogosResult VerifiedProxyImpl::ethBlockNumber() {
    return rpc("eth_blockNumber", json::array({}));
}

StdLogosResult VerifiedProxyImpl::ethGetBalance(const std::string& address, const std::string& blockTag) {
    return rpc("eth_getBalance", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetStorageAt(const std::string& address, const std::string& slot, const std::string& blockTag) {
    return rpc("eth_getStorageAt", json::array({address, slot, blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionCount(const std::string& address, const std::string& blockTag) {
    return rpc("eth_getTransactionCount", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetCode(const std::string& address, const std::string& blockTag) {
    return rpc("eth_getCode", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockByHash(const std::string& blockHash, bool fullTransactions) {
    return rpc("eth_getBlockByHash", json::array({blockHash, fullTransactions}));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockByNumber(const std::string& blockTag, bool fullTransactions) {
    return rpc("eth_getBlockByNumber", json::array({blockTag, fullTransactions}));
}

StdLogosResult VerifiedProxyImpl::ethGetUncleCountByBlockNumber(const std::string& blockTag) {
    return rpc("eth_getUncleCountByBlockNumber", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetUncleCountByBlockHash(const std::string& blockHash) {
    return rpc("eth_getUncleCountByBlockHash", json::array({blockHash}));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockTransactionCountByNumber(const std::string& blockTag) {
    return rpc("eth_getBlockTransactionCountByNumber", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockTransactionCountByHash(const std::string& blockHash) {
    return rpc("eth_getBlockTransactionCountByHash", json::array({blockHash}));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionByBlockNumberAndIndex(const std::string& blockTag, uint64_t index) {
    return rpc("eth_getTransactionByBlockNumberAndIndex", json::array({blockTag, index}));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionByBlockHashAndIndex(const std::string& blockHash, uint64_t index) {
    return rpc("eth_getTransactionByBlockHashAndIndex", json::array({blockHash, index}));
}

StdLogosResult VerifiedProxyImpl::ethCall(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("eth_call", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::ethCreateAccessList(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("eth_createAccessList", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::ethEstimateGas(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("eth_estimateGas", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionByHash(const std::string& txHash) {
    return rpc("eth_getTransactionByHash", json::array({txHash}));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockReceipts(const std::string& blockTag) {
    return rpc("eth_getBlockReceipts", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionReceipt(const std::string& txHash) {
    return rpc("eth_getTransactionReceipt", json::array({txHash}));
}

StdLogosResult VerifiedProxyImpl::ethGetLogs(const LogosMap& filterOptions) {
    return rpc("eth_getLogs", json::array({filterOptions}));
}

StdLogosResult VerifiedProxyImpl::ethNewFilter(const LogosMap& filterOptions) {
    return rpc("eth_newFilter", json::array({filterOptions}));
}

StdLogosResult VerifiedProxyImpl::ethUninstallFilter(const std::string& filterId) {
    return rpc("eth_uninstallFilter", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::ethGetFilterLogs(const std::string& filterId) {
    return rpc("eth_getFilterLogs", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::ethGetFilterChanges(const std::string& filterId) {
    return rpc("eth_getFilterChanges", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::ethBlobBaseFee() {
    return rpc("eth_blobBaseFee", json::array({}));
}

StdLogosResult VerifiedProxyImpl::ethGasPrice() {
    return rpc("eth_gasPrice", json::array({}));
}

StdLogosResult VerifiedProxyImpl::ethMaxPriorityFeePerGas() {
    return rpc("eth_maxPriorityFeePerGas", json::array({}));
}

StdLogosResult VerifiedProxyImpl::ethFeeHistory(uint64_t blockCount, const std::string& newestBlock, const LogosList& rewardPercentiles) {
    return rpc("eth_feeHistory", json::array({blockCount, newestBlock, rewardPercentiles}));
}

StdLogosResult VerifiedProxyImpl::ethSendRawTransaction(const std::string& txHexBytes) {
    return rpc("eth_sendRawTransaction", json::array({txHexBytes}));
}

StdLogosResult VerifiedProxyImpl::opChainId() {
    return rpc("op_chainId", json::array({}));
}

StdLogosResult VerifiedProxyImpl::opBlockNumber() {
    return rpc("op_blockNumber", json::array({}));
}

StdLogosResult VerifiedProxyImpl::opGetBalance(const std::string& address, const std::string& blockTag) {
    return rpc("op_getBalance", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetStorageAt(const std::string& address, const std::string& slot, const std::string& blockTag) {
    return rpc("op_getStorageAt", json::array({address, slot, blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetTransactionCount(const std::string& address, const std::string& blockTag) {
    return rpc("op_getTransactionCount", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetCode(const std::string& address, const std::string& blockTag) {
    return rpc("op_getCode", json::array({address, blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetBlockByHash(const std::string& blockHash, bool fullTransactions) {
    return rpc("op_getBlockByHash", json::array({blockHash, fullTransactions}));
}

StdLogosResult VerifiedProxyImpl::opGetBlockByNumber(const std::string& blockTag, bool fullTransactions) {
    return rpc("op_getBlockByNumber", json::array({blockTag, fullTransactions}));
}

StdLogosResult VerifiedProxyImpl::opGetUncleCountByBlockNumber(const std::string& blockTag) {
    return rpc("op_getUncleCountByBlockNumber", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetUncleCountByBlockHash(const std::string& blockHash) {
    return rpc("op_getUncleCountByBlockHash", json::array({blockHash}));
}

StdLogosResult VerifiedProxyImpl::opGetBlockTransactionCountByNumber(const std::string& blockTag) {
    return rpc("op_getBlockTransactionCountByNumber", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetBlockTransactionCountByHash(const std::string& blockHash) {
    return rpc("op_getBlockTransactionCountByHash", json::array({blockHash}));
}

StdLogosResult VerifiedProxyImpl::opGetTransactionByBlockNumberAndIndex(const std::string& blockTag, uint64_t index) {
    return rpc("op_getTransactionByBlockNumberAndIndex", json::array({blockTag, index}));
}

StdLogosResult VerifiedProxyImpl::opGetTransactionByBlockHashAndIndex(const std::string& blockHash, uint64_t index) {
    return rpc("op_getTransactionByBlockHashAndIndex", json::array({blockHash, index}));
}

StdLogosResult VerifiedProxyImpl::opCall(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("op_call", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::opCreateAccessList(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("op_createAccessList", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::opEstimateGas(const LogosMap& txArgs, const std::string& blockTag, bool optimisticStateFetch) {
    return rpc("op_estimateGas", json::array({txArgs, blockTag, optimisticStateFetch}));
}

StdLogosResult VerifiedProxyImpl::opGetTransactionByHash(const std::string& txHash) {
    return rpc("op_getTransactionByHash", json::array({txHash}));
}

StdLogosResult VerifiedProxyImpl::opGetBlockReceipts(const std::string& blockTag) {
    return rpc("op_getBlockReceipts", json::array({blockTag}));
}

StdLogosResult VerifiedProxyImpl::opGetTransactionReceipt(const std::string& txHash) {
    return rpc("op_getTransactionReceipt", json::array({txHash}));
}

StdLogosResult VerifiedProxyImpl::opGetLogs(const LogosMap& filterOptions) {
    return rpc("op_getLogs", json::array({filterOptions}));
}

StdLogosResult VerifiedProxyImpl::opNewFilter(const LogosMap& filterOptions) {
    return rpc("op_newFilter", json::array({filterOptions}));
}

StdLogosResult VerifiedProxyImpl::opUninstallFilter(const std::string& filterId) {
    return rpc("op_uninstallFilter", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::opGetFilterLogs(const std::string& filterId) {
    return rpc("op_getFilterLogs", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::opGetFilterChanges(const std::string& filterId) {
    return rpc("op_getFilterChanges", json::array({filterId}));
}

StdLogosResult VerifiedProxyImpl::opBlobBaseFee() {
    return rpc("op_blobBaseFee", json::array({}));
}

StdLogosResult VerifiedProxyImpl::opGasPrice() {
    return rpc("op_gasPrice", json::array({}));
}

StdLogosResult VerifiedProxyImpl::opMaxPriorityFeePerGas() {
    return rpc("op_maxPriorityFeePerGas", json::array({}));
}

StdLogosResult VerifiedProxyImpl::opFeeHistory(uint64_t blockCount, const std::string& newestBlock, const LogosList& rewardPercentiles) {
    return rpc("op_feeHistory", json::array({blockCount, newestBlock, rewardPercentiles}));
}

StdLogosResult VerifiedProxyImpl::opSendRawTransaction(const std::string& txHexBytes) {
    return rpc("op_sendRawTransaction", json::array({txHexBytes}));
}

// END GENERATED RPC WRAPPERS
