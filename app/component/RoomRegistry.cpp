/**
 * @file RoomRegistry.cpp
 * @brief 房间注册表实现
 */
#include "RoomRegistry.h"
#include <algorithm>
#include <chrono>

namespace yunyi {

void RoomRegistry::addRoom(
    uint32_t roomId, const std::string& roomName,
    uint16_t assignedPort, uint16_t localMcPort,
    const std::string& connectionCode) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    RoomRecord record;
    record.roomId         = roomId;
    record.roomName       = roomName;
    record.connectionCode = connectionCode;
    record.assignedPort   = assignedPort;
    record.localMcPort    = localMcPort;
    record.createdAt      = now;
    record.lastHeartbeat  = now;  // 初始心跳时间 = 创建时间

    _rooms[roomId]              = record;
    _portIndex[assignedPort]    = roomId;  // 双向索引
}

bool RoomRegistry::removeRoom(uint32_t roomId) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return false;

    _portIndex.erase(it->second.assignedPort);
    it->second.active = false;
    _rooms.erase(it);
    return true;
}

RoomRecord* RoomRegistry::findRoom(uint32_t roomId) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return nullptr;
    return &it->second;
}

RoomRecord* RoomRegistry::findByPort(uint16_t port) {
    std::lock_guard<std::mutex> lock(_mutex);
    // port -> roomId -> RoomRecord
    auto it = _portIndex.find(port);
    if (it == _portIndex.end()) return nullptr;
    auto rit = _rooms.find(it->second);
    if (rit == _rooms.end()) return nullptr;
    return &rit->second;
}

std::vector<RoomRecord> RoomRegistry::listActive() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<RoomRecord> result;
    for (const auto& kv : _rooms) {
        if (kv.second.active) {
            result.push_back(kv.second);
        }
    }
    return result;
}

void RoomRegistry::updateHeartbeat(
    uint32_t roomId, uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _rooms.find(roomId);
    if (it != _rooms.end()) {
        it->second.lastHeartbeat = timestamp;
    }
}

std::vector<uint32_t> RoomRegistry::findTimeoutRooms(
    uint64_t now, uint64_t timeoutMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<uint32_t> result;
    uint64_t timeoutSeconds = timeoutMs / 1000;

    for (const auto& kv : _rooms) {
        if (kv.second.active
            && (now - kv.second.lastHeartbeat > timeoutSeconds)) {
            result.push_back(kv.first);
        }
    }
    return result;
}

size_t RoomRegistry::activeCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _rooms.size();
}

} // namespace yunyi
