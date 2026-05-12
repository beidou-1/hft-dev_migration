#include "shm/shm_manager.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <string>

namespace hft {

static bool open_shm(const std::string& name, int flags, size_t size,
                     int prot, int& fd_out, void*& ptr_out, bool truncate) {
    fd_out = ::shm_open(name.c_str(), flags, 0600);
    if (fd_out < 0) return false;
    if (truncate && ::ftruncate(fd_out, static_cast<off_t>(size)) < 0) {
        ::close(fd_out); fd_out = -1; return false;
    }
    ptr_out = ::mmap(nullptr, size, prot, MAP_SHARED, fd_out, 0);
    if (ptr_out == MAP_FAILED) {
        ::close(fd_out); fd_out = -1; return false;
    }
    return true;
}

bool ShmManager::create() {
    void* p_cfg  = nullptr;
    void* p_data = nullptr;

    if (!open_shm(name_ + "_cfg",  O_CREAT | O_RDWR | O_TRUNC,
                  sizeof(StrategyConfig), PROT_READ | PROT_WRITE,
                  fd_config_, p_cfg, true))
        return false;

    if (!open_shm(name_ + "_data", O_CREAT | O_RDWR | O_TRUNC,
                  sizeof(TradeData), PROT_READ | PROT_WRITE,
                  fd_data_, p_data, true)) {
        ::munmap(p_cfg, sizeof(StrategyConfig));
        ::close(fd_config_); fd_config_ = -1;
        return false;
    }

    std::memset(p_cfg,  0, sizeof(StrategyConfig));
    std::memset(p_data, 0, sizeof(TradeData));
    config_ = new (p_cfg)  StrategyConfig{};
    data_   = new (p_data) TradeData{};
    owner_  = true;
    return true;
}

bool ShmManager::attach() {
    void* p_cfg  = nullptr;
    void* p_data = nullptr;

    if (!open_shm(name_ + "_cfg",  O_RDONLY, sizeof(StrategyConfig),
                  PROT_READ, fd_config_, p_cfg, false))
        return false;

    if (!open_shm(name_ + "_data", O_RDWR, sizeof(TradeData),
                  PROT_READ | PROT_WRITE, fd_data_, p_data, false)) {
        ::munmap(p_cfg, sizeof(StrategyConfig));
        ::close(fd_config_); fd_config_ = -1;
        return false;
    }

    config_ = reinterpret_cast<StrategyConfig*>(p_cfg);
    data_   = reinterpret_cast<TradeData*>(p_data);
    return true;
}

void ShmManager::detach() {
    if (config_) { ::munmap(config_, sizeof(StrategyConfig)); config_ = nullptr; }
    if (data_)   { ::munmap(data_,   sizeof(TradeData));   data_   = nullptr; }
    if (fd_config_ >= 0) { ::close(fd_config_); fd_config_ = -1; }
    if (fd_data_   >= 0) { ::close(fd_data_);   fd_data_   = -1; }
    if (owner_) {
        ::shm_unlink((name_ + "_cfg").c_str());
        ::shm_unlink((name_ + "_data").c_str());
        owner_ = false;
    }
}

bool ShmManager::push_slippage(const SlipPageEvent& ev)   { return data_->slippages.push(ev); }
bool ShmManager::push_hedge_fail(const HedgeFailEvent& ev) { return data_->hedge_fails.push(ev); }
bool ShmManager::push_latency(const LatencyEvent& ev)      { return data_->latencies.push(ev); }

bool ShmManager::pop_slippage(SlipPageEvent& ev)    { return data_->slippages.pop(ev); }
bool ShmManager::pop_hedge_fail(HedgeFailEvent& ev)  { return data_->hedge_fails.pop(ev); }
bool ShmManager::pop_latency(LatencyEvent& ev)       { return data_->latencies.pop(ev); }

void ShmManager::write_config(StrategyConfig& src) {
    assert(owner_ && "write_config called on non-owner (attach) instance");
    const uint32_t v = config_->version.load(std::memory_order_relaxed);
    config_->version.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    constexpr size_t offset = sizeof(std::atomic<uint32_t>);
    std::memcpy(reinterpret_cast<char*>(config_) + offset,
                reinterpret_cast<const char*>(&src) + offset,
                sizeof(StrategyConfig) - offset);
    std::atomic_thread_fence(std::memory_order_release);
    config_->version.store(v + 2, std::memory_order_release);
}

bool ShmManager::poll_config(StrategyConfig& snap) {
    const uint32_t v1 = config_->version.load(std::memory_order_acquire);
    if (v1 & 1) return false;
    if (v1 == last_config_version_) return false;

    constexpr size_t offset = sizeof(std::atomic<uint32_t>);
    std::memcpy(reinterpret_cast<char*>(&snap) + offset,
                reinterpret_cast<const char*>(config_) + offset,
                sizeof(StrategyConfig) - offset);
    std::atomic_thread_fence(std::memory_order_acquire);

    if (config_->version.load(std::memory_order_acquire) != v1) return false;

    snap.version.store(v1, std::memory_order_relaxed);
    last_config_version_ = v1;
    return true;
}

} // namespace hft
