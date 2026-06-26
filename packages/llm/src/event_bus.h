#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <cstdint>

namespace opencode {

class EventBus {
public:
    using Handler = std::function<void(const std::string& frame)>;

    uint64_t subscribe(const std::string& topic, Handler handler) {
        std::unique_lock lock(mutex_);
        auto id = next_id_++;
        topics_[topic][id] = std::move(handler);
        return id;
    }

    void unsubscribe(const std::string& topic, uint64_t id) {
        std::unique_lock lock(mutex_);
        auto it = topics_.find(topic);
        if (it == topics_.end()) return;
        it->second.erase(id);
        if (it->second.empty()) topics_.erase(it);
    }

    void publish(const std::string& topic, const std::string& frame) {
        std::shared_lock lock(mutex_);
        auto it = topics_.find(topic);
        if (it == topics_.end()) return;
        for (auto& [id, handler] : it->second) {
            handler(frame);
        }
    }

    void clear(const std::string& topic) {
        std::unique_lock lock(mutex_);
        topics_.erase(topic);
    }

    size_t subscriber_count(const std::string& topic) const {
        std::shared_lock lock(mutex_);
        auto it = topics_.find(topic);
        return it == topics_.end() ? 0 : it->second.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<uin t64_t, Handler>> topics_;
    std::atomic<uint64_t> next_id_{1};
};

} // namespace opencode
