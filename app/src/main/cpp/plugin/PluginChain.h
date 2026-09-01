#ifndef GUITARRACKCRAFT_PLUGIN_CHAIN_H
#define GUITARRACKCRAFT_PLUGIN_CHAIN_H

#include "IPlugin.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace guitarrackcraft {

class PluginChain {
public:
    static constexpr uint32_t kMaxSupportedPdcFrames = 65535;
    static constexpr uint32_t kMaxRetiredPlans = 8;
    static constexpr uint32_t kMaxMidiEvents = 128;

    PluginChain() = default;
    ~PluginChain();

    int addPlugin(std::unique_ptr<IPlugin> plugin, int position = -1);
    bool removePlugin(int index);
    bool reorderPlugins(int fromIndex, int toIndex);

    uint32_t process(const float* const* inputs,
                     float* const* outputs,
                     uint32_t numFrames,
                     const AudioProcessContext& context,
                     const MidiEvent* inputEvents,
                     uint32_t inputCount,
                     MidiEvent* outputEvents,
                     uint32_t outputCapacity);

    bool isEmptyForAudio() const noexcept {
        return pluginCount_.load(std::memory_order_acquire) == 0;
    }

    void setSampleRate(float sampleRate, uint32_t bufferSize = 0);
    void activate();
    void deactivate();

    uint32_t getLatencyFrames() const noexcept {
        return latencyFrames_.load(std::memory_order_relaxed);
    }
    bool hasLatencyOverflow() const noexcept {
        return latencyOverflow_.load(std::memory_order_relaxed);
    }
    std::string getRealtimeDiagnostic() const;

    size_t getSize() const;
    uint64_t getPluginInstanceId(int index) const;

    bool submitParameter(uint64_t instanceId, uint32_t portIndex, float value);
    float getParameter(uint64_t instanceId, uint32_t portIndex) const;
    bool getParameters(uint64_t instanceId,
                       const uint32_t* portIndices,
                       size_t count,
                       float* values) const;
    std::string getParameterDisplay(uint64_t instanceId, uint32_t portIndex) const;

    PluginRealtimeCounters getRealtimeCounters() const noexcept;
    uint64_t getMidiEventDrops() const noexcept {
        return midiEventDrops_.load(std::memory_order_relaxed);
    }
    uint64_t getPlanPublishDeferrals() const noexcept {
        return planPublishDeferrals_.load(std::memory_order_relaxed);
    }

    bool setManualLatencyFrames(int index, uint32_t frames);
    uint32_t getManualLatencyFrames(int index) const;
    uint32_t getRemainingPdcFrames(int index) const;
    uint32_t getPluginLatencyFrames(int index) const noexcept;
    uint32_t getPluginEffectiveLatencyFrames(int index) const noexcept;

    template<class Callback>
    decltype(auto) visitPlugin(size_t index, Callback&& callback) {
        std::shared_ptr<IPlugin> plugin;
        {
            std::lock_guard lock(controlMutex_);
            if (index < plugins_.size()) plugin = plugins_[index].plugin;
        }
        using Result = std::invoke_result_t<Callback, IPlugin&>;
        if (!plugin) {
            if constexpr (std::is_void_v<Result>) return;
            else return Result{};
        }
        return std::forward<Callback>(callback)(*plugin);
    }

    template<class Callback>
    decltype(auto) visitPlugin(size_t index, Callback&& callback) const {
        std::shared_ptr<IPlugin> plugin;
        {
            std::lock_guard lock(controlMutex_);
            if (index < plugins_.size()) plugin = plugins_[index].plugin;
        }
        using Result = std::invoke_result_t<Callback, const IPlugin&>;
        if (!plugin) {
            if constexpr (std::is_void_v<Result>) return;
            else return Result{};
        }
        return std::forward<Callback>(callback)(static_cast<const IPlugin&>(*plugin));
    }

    template<class Callback>
    bool visitPluginInstance(uint64_t instanceId, Callback&& callback) {
        std::shared_ptr<IPlugin> plugin;
        {
            std::lock_guard lock(controlMutex_);
            for (const auto& slot : plugins_) {
                if (slot.instanceId == instanceId) {
                    plugin = slot.plugin;
                    break;
                }
            }
        }
        if (!plugin) return false;
        std::forward<Callback>(callback)(*plugin);
        return true;
    }

    void setPluginFilePath(int index, const std::string& propertyUri, const std::string& path);
    bool injectAtom(uint64_t instanceId,
                    uint32_t portIndex,
                    const void* data,
                    uint32_t size);

    struct ChainState {
        std::vector<PluginState> plugins;
    };
    ChainState saveChainState();
    bool restorePluginState(int index, const PluginState& state);

private:
    struct PluginSlot {
        uint64_t instanceId = 0;
        std::shared_ptr<IPlugin> plugin;
        PluginInfo info;
        PluginState completedState;
        uint32_t manualLatencyFrames = 0;
    };
    struct ProcessPlan {
        uint64_t generation = 0;
        uint32_t manualLatencyFrames = 0;
        std::vector<IPlugin*> entries;
    };

    struct RetiredPlan {
        std::unique_ptr<ProcessPlan> plan;
        uint64_t retireEpoch = 0;
    };

    struct AudioHazard {
        PluginChain* owner;
        ~AudioHazard() {
            owner->audioHazard_.store(nullptr, std::memory_order_release);
            owner->audioEpoch_.fetch_add(1, std::memory_order_release);
        }
    };
    bool publishPlanLocked(std::unique_ptr<ProcessPlan> plan);
    bool rebuildPlanLocked();
    void reclaimRetiredLocked();
    void reclaimLoop();
    void recomputeLatencyLocked() noexcept;
    void ensureBuffers(uint32_t frames, uint32_t channels);
    void clearOutputs(float* const* outputs, uint32_t frames) const noexcept;
    std::shared_ptr<IPlugin> pluginAtLocked(int index) const;
    std::shared_ptr<IPlugin> pluginByIdLocked(uint64_t instanceId) const;

    std::vector<PluginSlot> plugins_;
    mutable std::mutex controlMutex_;

    std::atomic<ProcessPlan*> activePlan_{nullptr};
    std::array<RetiredPlan, kMaxRetiredPlans> retiredPlans_{};
    std::atomic<ProcessPlan*> audioHazard_{nullptr};
    std::atomic<uint64_t> audioEpoch_{0};
    uint64_t generation_ = 0;
    std::atomic<uint32_t> pluginCount_{0};
    std::atomic<uint32_t> latencyFrames_{0};
    std::atomic<uint32_t> bufferSize_{0};
    std::atomic<uint32_t> renderBufferSize_{0};
    std::atomic<bool> latencyOverflow_{false};
    std::atomic<uint64_t> oversizedBlocks_{0};
    std::atomic<uint64_t> midiEventDrops_{0};
    std::atomic<uint64_t> planPublishDeferrals_{0};

    bool running_ = false;
    bool deactivationPending_ = false;
    std::string realtimeDiagnostic_;
    bool planPublished_ = false;
    float sampleRate_ = 0.0f;

    std::array<MidiEvent, kMaxMidiEvents> midiScratchA_{};
    std::array<MidiEvent, kMaxMidiEvents> midiScratchB_{};
    std::vector<std::vector<float>> intermediateBuffers_;

    std::mutex reclaimerMutex_;
    std::condition_variable reclaimerCond_;
    std::thread reclaimerThread_;
    bool reclaimerStop_ = false;
    std::deque<std::shared_ptr<IPlugin>> retiredPlugins_;
    bool pluginsActivated_ = false;
};

} // namespace guitarrackcraft

#endif
