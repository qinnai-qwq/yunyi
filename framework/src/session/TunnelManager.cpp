/**
 * @file TunnelManager.cpp
 * @brief 数据隧道生命周期管理实现
 */
#include "yunyi/session/TunnelManager.h"
#include "yunyi/session/Session.h"

namespace yunyi {

TunnelInfo* TunnelManager::createPendingTunnel(
    uint32_t playerConnId, Session* playerSession) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto info = std::make_unique<TunnelInfo>();
    info->playerConnId  = playerConnId;
    info->playerSession = playerSession;
    info->paired        = false;

    auto* raw = info.get();
    _tunnels[playerConnId] = std::move(info);
    return raw;
}

TunnelInfo* TunnelManager::pairTunnel(
    uint32_t playerConnId, Session* tunnelSession) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _tunnels.find(playerConnId);
    if (it == _tunnels.end()) return nullptr;
    // 已配对的隧道不允许重复配对
    if (it->second->paired) return nullptr;

    it->second->tunnelSession = tunnelSession;
    it->second->paired        = true;
    return it->second.get();
}

void TunnelManager::removeTunnel(uint32_t playerConnId) {
    std::unique_ptr<TunnelInfo> info;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _tunnels.find(playerConnId);
        if (it == _tunnels.end()) return;
        info = std::move(it->second);
        _tunnels.erase(it);
    }

    // 释放双端 Session（不加锁，避免回调中死锁）
    if (info->playerSession) {
        info->playerSession->close();
        info->playerSession->release();
    }
    if (info->tunnelSession) {
        info->tunnelSession->close();
        info->tunnelSession->release();
    }
}

TunnelInfo* TunnelManager::findTunnel(uint32_t playerConnId) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tunnels.find(playerConnId);
    if (it == _tunnels.end()) return nullptr;
    return it->second.get();
}

size_t TunnelManager::activeCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _tunnels.size();
}

uint64_t TunnelManager::totalBytesRelayed() const {
    std::lock_guard<std::mutex> lock(_mutex);
    uint64_t total = 0;
    for (const auto& kv : _tunnels) {
        total += kv.second->bytesRelayed;
    }
    return total;
}

std::vector<TunnelInfo> TunnelManager::snapshot() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<TunnelInfo> result;
    result.reserve(_tunnels.size());
    for (const auto& kv : _tunnels) {
        result.push_back(*kv.second);
    }
    return result;
}

} // namespace yunyi
