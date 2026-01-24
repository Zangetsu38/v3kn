// v3knr project
// Copyright (C) 2026 Vita3K team

#pragma once

#include <initializer_list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

extern std::shared_mutex account_lifecycle_mutex;

std::shared_ptr<std::shared_mutex> get_resource_mutex(const std::string &key);

class ResourceLock {
    std::vector<std::shared_ptr<std::shared_mutex>> mutexes;
    std::vector<std::unique_lock<std::shared_mutex>> locks;

public:
    explicit ResourceLock(const std::string &key);
    explicit ResourceLock(std::initializer_list<std::string> keys);
};

class ResourceReadLock {
    std::shared_ptr<std::shared_mutex> mutex;
    std::shared_lock<std::shared_mutex> lock;

public:
    explicit ResourceReadLock(const std::string &key);
};