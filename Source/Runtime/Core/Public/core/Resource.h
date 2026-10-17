#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
class Resource {
public:
    explicit Resource(std::string name = {}) : name_(std::move(name)) {}
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    virtual ~Resource() = default;
    virtual std::string_view TypeName() const { return "Resource"; }
    std::uint64_t Id() const { return id_; }
    const std::string& Name() const { return name_; }

private:
    friend class ResourceStore;
    std::uint64_t id_{}; // 由显式资源所有者分配的进程内标识。
    std::string name_; // 独立于 ECS 实体名称的作者资源标签。
};

template<class T>
struct ResourceHandle {
    std::uint64_t value{}; // 非拥有型进程内标识，零始终无效。
    explicit operator bool() const { return value != 0; }
    bool operator==(const ResourceHandle&) const = default;
};

// 所有权边界显式确定，句柄不延长资源寿命或决定卸载时机。
class ResourceStore {
public:
    ResourceStore() = default;
    ResourceStore(const ResourceStore&) = delete;
    ResourceStore& operator=(const ResourceStore&) = delete;
    ResourceStore(ResourceStore&&) = default;
    ResourceStore& operator=(ResourceStore&&) = default;

    // 标识永不复用，清空所有者后旧句柄不会重新变为有效。
    template<class T, class... Arguments>
    ResourceHandle<T> Create(Arguments&&... arguments) {
        static_assert(std::is_base_of_v<Resource, T>);
        auto resource = std::make_unique<T>(std::forward<Arguments>(arguments)...);
        auto next = nextId_.load(std::memory_order_relaxed);
        do {
            if (next == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("resource identity capacity exhausted");
        } while (!nextId_.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));
        resource->id_ = next;
        resources_.emplace(next, std::move(resource));
        return {next};
    }

    // 暴露借用指针之前同时检查所有权和具体类型。
    template<class T> T* Resolve(ResourceHandle<T> handle) {
        static_assert(std::is_base_of_v<Resource, T>);
        const auto found = resources_.find(handle.value);
        return found == resources_.end() ? nullptr : dynamic_cast<T*>(found->second.get());
    }
    template<class T> const T* Resolve(ResourceHandle<T> handle) const {
        static_assert(std::is_base_of_v<Resource, T>);
        const auto found = resources_.find(handle.value);
        return found == resources_.end() ? nullptr : dynamic_cast<const T*>(found->second.get());
    }
    template<class T> bool Erase(ResourceHandle<T> handle) {
        return Resolve(handle) && resources_.erase(handle.value) != 0;
    }
    void Clear() { resources_.clear(); }
    std::size_t Size() const { return resources_.size(); }
    // 枚举仅借用存活资源；清空所有者后这些指针立即失效。
    std::vector<const Resource*> All() const {
        std::vector<const Resource*> result;
        result.reserve(resources_.size());
        for (const auto& [id, resource] : resources_) {
            (void)id;
            result.push_back(resource.get());
        }
        std::sort(result.begin(), result.end(), [](const Resource* a, const Resource* b) {
            if (a->Name() != b->Name()) return a->Name() < b->Name();
            if (a->TypeName() != b->TypeName()) return a->TypeName() < b->TypeName();
            return a->Id() < b->Id();
        });
        return result;
    }

private:
    std::unordered_map<std::uint64_t, std::unique_ptr<Resource>> resources_; // 资源对象的唯一所有者。
    inline static std::atomic<std::uint64_t> nextId_{1}; // 所有存储与资源类型共用的进程计数器。
};
}
