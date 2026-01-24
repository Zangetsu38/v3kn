// v3knr project
// Copyright (C) 2026 Vita3K team

#include <utils/concurrency.h>

#include <algorithm>
#include <unordered_map>

std::shared_mutex account_lifecycle_mutex;

std::shared_ptr<std::shared_mutex> get_resource_mutex(const std::string &key) {
    static std::mutex registry_mutex;
    static std::unordered_map<std::string, std::weak_ptr<std::shared_mutex>> registry;
    std::lock_guard<std::mutex> lock(registry_mutex);

    auto &entry = registry[key];
    auto mutex = entry.lock();
    if (!mutex) {
        mutex = std::make_shared<std::shared_mutex>();
        entry = mutex;
    }
    std::erase_if(registry, [](const auto &item) { return item.second.expired(); });
    return mutex;
}

ResourceLock::ResourceLock(const std::string &key)
    : ResourceLock({ key }) {
}

ResourceLock::ResourceLock(std::initializer_list<std::string> keys) {
    std::vector<std::string> ordered_keys(keys);
    std::sort(ordered_keys.begin(), ordered_keys.end());
    ordered_keys.erase(std::unique(ordered_keys.begin(), ordered_keys.end()), ordered_keys.end());
    for (const auto &key : ordered_keys) {
        mutexes.push_back(get_resource_mutex(key));
    }
    // All multi-resource transactions acquire locks in the same order.
    for (const auto &mutex : mutexes) {
        locks.emplace_back(*mutex);
    }
}

ResourceReadLock::ResourceReadLock(const std::string &key)
    : mutex(get_resource_mutex(key))
    , lock(*mutex) {
}