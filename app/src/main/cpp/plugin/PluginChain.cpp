#include "PluginChain.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {
std::atomic<uint64_t> gNextPluginInstanceId{1};

uint64_t nextPluginInstanceId() {
    const uint64_t id = gNextPluginInstanceId.fetch_add(1, std::memory_order_relaxed);
    return id != 0 ? id : gNextPluginInstanceId.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

namespace guitarrackcraft {

PluginChain::~PluginChain() {
    deactivate();
    for (unsigned attempt = 0; attempt < 250; ++attempt) {
        {
            std::lock_guard lock(controlMutex_);
            if (!deactivationPending_ &&
                (audioEpoch_.load(std::memory_order_acquire) & 1u) == 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::lock_guard lock(reclaimerMutex_);
        reclaimerStop_ = true;
    }
    reclaimerCond_.notify_one();
    if (reclaimerThread_.joinable()) reclaimerThread_.join();

    std::vector<std::shared_ptr<IPlugin>> remaining;
    {
        std::lock_guard lock(controlMutex_);
        ProcessPlan* active = activePlan_.exchange(nullptr, std::memory_order_acq_rel);
        delete active;
        for (auto& retired : retiredPlans_) retired.plan.reset();
        while (!retiredPlugins_.empty()) {
            remaining.push_back(std::move(retiredPlugins_.front()));
            retiredPlugins_.pop_front();
        }
        if (pluginsActivated_ || deactivationPending_) {
            for (const auto& slot : plugins_) remaining.push_back(slot.plugin);
        }
        plugins_.clear();
        pluginsActivated_ = false;
        deactivationPending_ = false;
    }
    for (const auto& plugin : remaining) {
        if (plugin) plugin->deactivate();
    }
}

std::shared_ptr<IPlugin> PluginChain::pluginAtLocked(int index) const {
    if (index < 0 || index >= static_cast<int>(plugins_.size())) return {};
    return plugins_[static_cast<size_t>(index)].plugin;
}

std::shared_ptr<IPlugin> PluginChain::pluginByIdLocked(uint64_t instanceId) const {
    for (const auto& slot : plugins_) {
        if (slot.instanceId == instanceId) return slot.plugin;
    }
    return {};
}

void PluginChain::reclaimRetiredLocked() {
    if (audioEpoch_.load(std::memory_order_acquire) & 1u) return;
    for (auto& retired : retiredPlans_) {
        retired.plan.reset();
        retired.retireEpoch = 0;
    }
}

bool PluginChain::publishPlanLocked(std::unique_ptr<ProcessPlan> plan) {
    reclaimRetiredLocked();
    ProcessPlan* old = activePlan_.load(std::memory_order_acquire);
    auto slot = retiredPlans_.end();
    if (old) {
        slot = std::find_if(retiredPlans_.begin(), retiredPlans_.end(),
                            [](const RetiredPlan& retired) { return !retired.plan; });
        if (slot == retiredPlans_.end()) {
            planPublishDeferrals_.fetch_add(1, std::memory_order_relaxed);
            realtimeDiagnostic_ = "plan-publication-busy";
            return false;
        }
    }
    plan->generation = generation_ + 1;
    activePlan_.store(plan.release(), std::memory_order_release);
    if (old) {
        slot->plan.reset(old);
        slot->retireEpoch = audioEpoch_.load(std::memory_order_acquire);
    }
    if (!reclaimerThread_.joinable()) {
        reclaimerThread_ = std::thread(&PluginChain::reclaimLoop, this);
    }
    reclaimerCond_.notify_one();
    return true;
}

bool PluginChain::rebuildPlanLocked() {
    auto plan = std::make_unique<ProcessPlan>();
    plan->entries.reserve(plugins_.size());
    uint64_t manualLatency = 0;
    for (const auto& slot : plugins_) {
        if (slot.info.realtimeClass == RealtimeClass::Unsupported) {
            realtimeDiagnostic_ = "plugin-not-certified-realtime";
            return false;
        }
        plan->entries.push_back(slot.plugin.get());
        manualLatency += slot.manualLatencyFrames;
    }
    plan->manualLatencyFrames = static_cast<uint32_t>(
            std::min<uint64_t>(manualLatency, UINT32_MAX));
    if (!publishPlanLocked(std::move(plan))) return false;
    realtimeDiagnostic_.clear();
    return true;
}

void PluginChain::reclaimLoop() {
    std::unique_lock wakeLock(reclaimerMutex_);
    while (!reclaimerStop_) {
        reclaimerCond_.wait_for(wakeLock, std::chrono::milliseconds(2));
        if (reclaimerStop_) break;
        wakeLock.unlock();

        std::vector<std::shared_ptr<IPlugin>> toDeactivate;
        {
            std::lock_guard controlLock(controlMutex_);
            reclaimRetiredLocked();
            if ((audioEpoch_.load(std::memory_order_acquire) & 1u) == 0) {
                while (!retiredPlugins_.empty()) {
                    toDeactivate.push_back(std::move(retiredPlugins_.front()));
                    retiredPlugins_.pop_front();
                }
                if (deactivationPending_) {
                    for (const auto& slot : plugins_) toDeactivate.push_back(slot.plugin);
                    deactivationPending_ = false;
                    pluginsActivated_ = false;
                }
            }
        }
        for (const auto& plugin : toDeactivate) {
            if (plugin) plugin->deactivate();
        }

        wakeLock.lock();
    }
}

int PluginChain::addPlugin(std::unique_ptr<IPlugin> plugin, int position) {
    if (!plugin) return -1;
    std::shared_ptr<IPlugin> prepared(std::move(plugin));

    float sampleRate = 0.0f;
    uint32_t quantum = 0;
    uint64_t baseGeneration = 0;
    {
        std::lock_guard lock(controlMutex_);
        if (deactivationPending_) return -1;
        sampleRate = sampleRate_;
        quantum = bufferSize_.load(std::memory_order_relaxed);
        baseGeneration = generation_;
    }

    try {
        prepared->prepare();
        if (sampleRate > 0.0f) prepared->activate(sampleRate, quantum);
    } catch (...) {
        return -1;
    }
    if (sampleRate > 0.0f && !prepared->isReadyForRealtime()) {
        prepared->deactivate();
        std::lock_guard lock(controlMutex_);
        realtimeDiagnostic_ = "plugin-activate-failed";
        return -1;
    }

    PluginInfo info;
    try {
        info = prepared->getInfo();
    } catch (...) {
        if (sampleRate > 0.0f) prepared->deactivate();
        return -1;
    }

    PluginState completed;
    completed.pluginUri = info.id;
    completed.format = info.format;

    int result = -1;
    bool accepted = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!deactivationPending_ && generation_ == baseGeneration) {
            const int index = position < 0 || position >= static_cast<int>(plugins_.size())
                    ? static_cast<int>(plugins_.size())
                    : position;
            PluginSlot slot;
            slot.instanceId = nextPluginInstanceId();
            slot.plugin = prepared;
            slot.info = std::move(info);
            slot.completedState = std::move(completed);
            plugins_.insert(plugins_.begin() + index, std::move(slot));

            bool publicationAccepted = true;
            if (planPublished_) {
                if (!running_ &&
                    plugins_[static_cast<size_t>(index)].info.realtimeClass ==
                        RealtimeClass::Unsupported) {
                    reclaimRetiredLocked();
                    auto retired = std::find_if(
                        retiredPlans_.begin(), retiredPlans_.end(),
                        [](const RetiredPlan& candidate) { return !candidate.plan; });
                    if (retired == retiredPlans_.end()) {
                        planPublishDeferrals_.fetch_add(1, std::memory_order_relaxed);
                        realtimeDiagnostic_ = "plan-retirement-busy";
                        publicationAccepted = false;
                    } else {
                        ProcessPlan* old =
                            activePlan_.exchange(nullptr, std::memory_order_acq_rel);
                        if (old) {
                            retired->plan.reset(old);
                            retired->retireEpoch =
                                audioEpoch_.load(std::memory_order_acquire);
                        }
                        planPublished_ = false;
                    }
                } else {
                    publicationAccepted = rebuildPlanLocked();
                }
            }
            if (publicationAccepted) {
                ++generation_;
                pluginCount_.store(static_cast<uint32_t>(plugins_.size()),
                                   std::memory_order_release);
                if (sampleRate > 0.0f) pluginsActivated_ = true;
                recomputeLatencyLocked();
                result = index;
                accepted = true;
            } else {
                plugins_.erase(plugins_.begin() + index);
            }
        }
    }

    if (!accepted && sampleRate > 0.0f) prepared->deactivate();
    return result;
}

bool PluginChain::removePlugin(int index) {
    std::lock_guard lock(controlMutex_);
    if (deactivationPending_ || index < 0 || index >= static_cast<int>(plugins_.size())) {
        return false;
    }

    PluginSlot removed = std::move(plugins_[static_cast<size_t>(index)]);
    plugins_.erase(plugins_.begin() + index);
    if (planPublished_ && !rebuildPlanLocked()) {
        plugins_.insert(plugins_.begin() + index, std::move(removed));
        return false;
    }

    ++generation_;
    pluginCount_.store(static_cast<uint32_t>(plugins_.size()), std::memory_order_release);
    recomputeLatencyLocked();
    retiredPlugins_.push_back(std::move(removed.plugin));
    if (!reclaimerThread_.joinable()) {
        reclaimerThread_ = std::thread(&PluginChain::reclaimLoop, this);
    }
    reclaimerCond_.notify_one();
    return true;
}

bool PluginChain::reorderPlugins(int fromIndex, int toIndex) {
    std::lock_guard lock(controlMutex_);
    if (deactivationPending_ || fromIndex < 0 || toIndex < 0 ||
        fromIndex >= static_cast<int>(plugins_.size()) ||
        toIndex >= static_cast<int>(plugins_.size()) || fromIndex == toIndex) {
        return false;
    }

    PluginSlot moved = std::move(plugins_[static_cast<size_t>(fromIndex)]);
    plugins_.erase(plugins_.begin() + fromIndex);
    plugins_.insert(plugins_.begin() + toIndex, std::move(moved));
    if (planPublished_ && !rebuildPlanLocked()) {
        PluginSlot restore = std::move(plugins_[static_cast<size_t>(toIndex)]);
        plugins_.erase(plugins_.begin() + toIndex);
        plugins_.insert(plugins_.begin() + fromIndex, std::move(restore));
        return false;
    }

    ++generation_;
    recomputeLatencyLocked();
    return true;
}

uint32_t PluginChain::process(const float* const* inputs,
                              float* const* outputs,
                              uint32_t numFrames,
                              const AudioProcessContext& context,
                              const MidiEvent* inputEvents,
                              uint32_t inputCount,
                              MidiEvent* outputEvents,
                              uint32_t outputCapacity) {
    if (inputCount > kMaxMidiEvents) {
        midiEventDrops_.fetch_add(inputCount - kMaxMidiEvents, std::memory_order_relaxed);
    }
    if (numFrames == 0) return 0;

    audioEpoch_.fetch_add(1, std::memory_order_acq_rel);
    ProcessPlan* plan = activePlan_.load(std::memory_order_acquire);
    audioHazard_.store(plan, std::memory_order_release);
    AudioHazard hazard{this};

    const uint32_t quantum = bufferSize_.load(std::memory_order_acquire);
    const uint32_t renderCapacity = renderBufferSize_.load(std::memory_order_acquire);
    if (!plan || quantum == 0 || numFrames > quantum || numFrames > renderCapacity ||
        !outputs || !outputs[0] || !outputs[1]) {
        if (numFrames > quantum || numFrames > renderCapacity) {
            oversizedBlocks_.fetch_add(1, std::memory_order_relaxed);
        }
        clearOutputs(outputs, numFrames);
        return 0;
    }

    if (plan->entries.empty()) {
        if (inputs && inputs[0] && inputs[1]) {
            if (outputs[0] != inputs[0]) {
                std::memcpy(outputs[0], inputs[0], numFrames * sizeof(float));
            }
            if (outputs[1] != inputs[1]) {
                std::memcpy(outputs[1], inputs[1], numFrames * sizeof(float));
            }
        } else {
            clearOutputs(outputs, numFrames);
        }
        latencyFrames_.store(plan->manualLatencyFrames, std::memory_order_relaxed);
        latencyOverflow_.store(plan->manualLatencyFrames > kMaxSupportedPdcFrames,
                               std::memory_order_relaxed);
        return 0;
    }

    if (!inputs || !inputs[0] || !inputs[1] || intermediateBuffers_.size() < 4) {
        clearOutputs(outputs, numFrames);
        return 0;
    }

    const MidiEvent* currentMidi = inputEvents;
    uint32_t currentMidiCount = std::min(inputCount, kMaxMidiEvents);
    MidiEvent* midiScratch = midiScratchA_.data();
    const float* currentInput[2] = {inputs[0], inputs[1]};
    uint32_t produced = 0;

    for (size_t index = 0; index < plan->entries.size(); ++index) {
        const bool last = index + 1 == plan->entries.size();
        const size_t pair = index & 1u;
        float* stageOutput[2] = {
            last ? outputs[0] : intermediateBuffers_[pair * 2].data(),
            last ? outputs[1] : intermediateBuffers_[pair * 2 + 1].data()
        };
        MidiEvent* stageMidi = last ? outputEvents : midiScratch;
        const uint32_t stageCapacity = last ? outputCapacity : kMaxMidiEvents;
        produced = std::min(plan->entries[index]->process(
                                    currentInput, stageOutput, numFrames, context,
                                    currentMidi, currentMidiCount, stageMidi, stageCapacity),
                            stageCapacity);
        if (produced == 0 && stageMidi && currentMidi && currentMidiCount > 0) {
            uint32_t forwarded = 0;
            const uint32_t limit = std::min(currentMidiCount, stageCapacity);
            for (uint32_t event = 0; event < limit; ++event) {
                if (currentMidi[event].frameOffset < numFrames) {
                    stageMidi[forwarded++] = currentMidi[event];
                }
            }
            produced = forwarded;
        }
        if (!last) {
            currentInput[0] = stageOutput[0];
            currentInput[1] = stageOutput[1];
            currentMidi = stageMidi;
            currentMidiCount = produced;
            midiScratch = midiScratch == midiScratchA_.data()
                    ? midiScratchB_.data()
                    : midiScratchA_.data();
        }
    }
    uint64_t dynamicLatency = plan->manualLatencyFrames;
    for (const auto* plugin : plan->entries) dynamicLatency += plugin->getLatencyFrames();
    latencyOverflow_.store(dynamicLatency > kMaxSupportedPdcFrames,
                           std::memory_order_relaxed);
    latencyFrames_.store(
            static_cast<uint32_t>(std::min<uint64_t>(dynamicLatency, UINT32_MAX)),
            std::memory_order_relaxed);
    return produced;
}

void PluginChain::setSampleRate(float sampleRate, uint32_t bufferSize) {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    {
        std::lock_guard lock(controlMutex_);
        reclaimRetiredLocked();
        if (running_ || deactivationPending_) {
            realtimeDiagnostic_ = "rate-change-requires-stopped-chain";
            return;
        }
        sampleRate_ = sampleRate;
        bufferSize_.store(bufferSize, std::memory_order_release);
        renderBufferSize_.store(bufferSize, std::memory_order_release);
        ensureBuffers(bufferSize, 4);
        for (const auto& slot : plugins_) plugins.push_back(slot.plugin);
        ++generation_;
    }

    std::vector<std::shared_ptr<IPlugin>> activated;
    bool activationFailed = false;
    try {
        for (const auto& plugin : plugins) {
            plugin->activate(sampleRate, bufferSize);
            activated.push_back(plugin);
            if (!plugin->isReadyForRealtime()) {
                activationFailed = true;
                break;
            }
        }
    } catch (...) {
        activationFailed = true;
    }
    if (activationFailed) {
        for (const auto& plugin : activated) plugin->deactivate();
        std::lock_guard lock(controlMutex_);
        realtimeDiagnostic_ = "plugin-activate-failed";
        pluginsActivated_ = false;
        return;
    }

    std::lock_guard lock(controlMutex_);
    pluginsActivated_ = sampleRate > 0.0f && !plugins.empty();
    if (sampleRate > 0.0f && bufferSize > 0) {
        planPublished_ = rebuildPlanLocked();
        if (planPublished_) ++generation_;
    } else {
        planPublished_ = false;
    }
    recomputeLatencyLocked();
}

void PluginChain::activate() {
    std::lock_guard lock(controlMutex_);
    reclaimRetiredLocked();
    if (deactivationPending_) {
        realtimeDiagnostic_ = "plugin-lifecycle-busy";
        return;
    }
    if (!reclaimerThread_.joinable()) {
        reclaimerThread_ = std::thread(&PluginChain::reclaimLoop, this);
    }
    if (running_) return;
    if (!planPublished_ && !rebuildPlanLocked()) return;
    planPublished_ = true;
    running_ = true;
    ++generation_;
}

void PluginChain::deactivate() {
    std::lock_guard lock(controlMutex_);
    if (!running_ && !planPublished_ && !pluginsActivated_ && !deactivationPending_) return;
    reclaimRetiredLocked();
    ProcessPlan* old = activePlan_.load(std::memory_order_acquire);
    auto slot = retiredPlans_.end();
    if (old) {
        slot = std::find_if(retiredPlans_.begin(), retiredPlans_.end(),
                            [](const RetiredPlan& retired) { return !retired.plan; });
        if (slot == retiredPlans_.end()) {
            planPublishDeferrals_.fetch_add(1, std::memory_order_relaxed);
            realtimeDiagnostic_ = "plan-retirement-busy";
            return;
        }
    }
    running_ = false;
    planPublished_ = false;
    old = activePlan_.exchange(nullptr, std::memory_order_acq_rel);
    if (old) {
        slot->plan.reset(old);
        slot->retireEpoch = audioEpoch_.load(std::memory_order_acquire);
    }
    deactivationPending_ = pluginsActivated_;
    ++generation_;
    latencyFrames_.store(0, std::memory_order_relaxed);
    latencyOverflow_.store(false, std::memory_order_relaxed);
    if (!reclaimerThread_.joinable()) {
        reclaimerThread_ = std::thread(&PluginChain::reclaimLoop, this);
    }
    reclaimerCond_.notify_one();
}

std::string PluginChain::getRealtimeDiagnostic() const {
    std::lock_guard lock(controlMutex_);
    return realtimeDiagnostic_;
}

size_t PluginChain::getSize() const {
    std::lock_guard lock(controlMutex_);
    return plugins_.size();
}

uint64_t PluginChain::getPluginInstanceId(int index) const {
    std::lock_guard lock(controlMutex_);
    return index >= 0 && index < static_cast<int>(plugins_.size())
            ? plugins_[static_cast<size_t>(index)].instanceId
            : 0;
}

bool PluginChain::submitParameter(uint64_t instanceId, uint32_t portIndex, float value) {
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginByIdLocked(instanceId);
    }
    if (!plugin) return false;
    plugin->setParameter(portIndex, value);
    return true;
}

float PluginChain::getParameter(uint64_t instanceId, uint32_t portIndex) const {
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginByIdLocked(instanceId);
    }
    return plugin ? plugin->getParameter(portIndex) : 0.0f;
}

bool PluginChain::getParameters(uint64_t instanceId,
                                const uint32_t* portIndices,
                                size_t count,
                                float* values) const {
    if ((count != 0 && (!portIndices || !values))) return false;
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginByIdLocked(instanceId);
    }
    if (!plugin) return false;
    for (size_t index = 0; index < count; ++index) {
        values[index] = plugin->getParameter(portIndices[index]);
    }
    return true;
}

std::string PluginChain::getParameterDisplay(uint64_t instanceId, uint32_t portIndex) const {
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginByIdLocked(instanceId);
    }
    return plugin ? plugin->getParameterDisplay(portIndex) : std::string{};
}

PluginRealtimeCounters PluginChain::getRealtimeCounters() const noexcept {
    std::vector<std::shared_ptr<IPlugin>> plugins;
    {
        std::lock_guard lock(controlMutex_);
        plugins.reserve(plugins_.size());
        for (const auto& slot : plugins_) plugins.push_back(slot.plugin);
    }
    PluginRealtimeCounters total;
    for (const auto& plugin : plugins) {
        const auto counters = plugin->getRealtimeCounters();
        total.inputStarvations += counters.inputStarvations;
        total.outputUnderrunFrames += counters.outputUnderrunFrames;
        total.guestDeadlineMisses += counters.guestDeadlineMisses;
    }
    return total;
}

bool PluginChain::setManualLatencyFrames(int index, uint32_t frames) {
    std::lock_guard lock(controlMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size()) ||
        frames > kMaxSupportedPdcFrames) {
        return false;
    }
    auto& slot = plugins_[static_cast<size_t>(index)];
    const uint32_t previous = slot.manualLatencyFrames;
    slot.manualLatencyFrames = frames;
    recomputeLatencyLocked();
    const bool overflow = latencyOverflow_.load(std::memory_order_relaxed);
    if (overflow || (planPublished_ && !rebuildPlanLocked())) {
        slot.manualLatencyFrames = previous;
        recomputeLatencyLocked();
        return false;
    }
    ++generation_;
    return true;
}

uint32_t PluginChain::getManualLatencyFrames(int index) const {
    std::lock_guard lock(controlMutex_);
    return index >= 0 && index < static_cast<int>(plugins_.size())
            ? plugins_[static_cast<size_t>(index)].manualLatencyFrames
            : 0;
}

uint32_t PluginChain::getRemainingPdcFrames(int index) const {
    std::lock_guard lock(controlMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) return 0;
    uint64_t used = plugins_[static_cast<size_t>(index)].plugin->getLatencyFrames();
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (static_cast<int>(i) == index) continue;
        used += plugins_[i].plugin->getLatencyFrames();
        used += plugins_[i].manualLatencyFrames;
    }
    return used >= kMaxSupportedPdcFrames
            ? 0
            : static_cast<uint32_t>(kMaxSupportedPdcFrames - used);
}

uint32_t PluginChain::getPluginLatencyFrames(int index) const noexcept {
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginAtLocked(index);
    }
    return plugin ? plugin->getLatencyFrames() : 0;
}

uint32_t PluginChain::getPluginEffectiveLatencyFrames(int index) const noexcept {
    std::lock_guard lock(controlMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) return 0;
    const auto& slot = plugins_[static_cast<size_t>(index)];
    return slot.plugin->getLatencyFrames() + slot.manualLatencyFrames;
}

void PluginChain::setPluginFilePath(int index,
                                    const std::string& propertyUri,
                                    const std::string& path) {
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginAtLocked(index);
    }
    if (plugin) plugin->setFilePath(propertyUri, path);
}

bool PluginChain::injectAtom(uint64_t instanceId,
                             uint32_t portIndex,
                             const void* data,
                             uint32_t size) {
    (void)portIndex;
    std::shared_ptr<IPlugin> plugin;
    {
        std::lock_guard lock(controlMutex_);
        plugin = pluginByIdLocked(instanceId);
    }
    if (!plugin) return false;
    plugin->injectAtom(data, size);
    return true;
}

PluginChain::ChainState PluginChain::saveChainState() {
    struct SaveItem {
        uint64_t instanceId;
        std::shared_ptr<IPlugin> plugin;
        PluginInfo info;
        PluginState completed;
        uint32_t manualLatencyFrames;
    };

    std::vector<SaveItem> items;
    bool running = false;
    {
        std::lock_guard lock(controlMutex_);
        running = running_;
        items.reserve(plugins_.size());
        for (const auto& slot : plugins_) {
            items.push_back({slot.instanceId, slot.plugin, slot.info,
                             slot.completedState, slot.manualLatencyFrames});
        }
    }

    ChainState chain;
    chain.plugins.reserve(items.size());
    for (auto& item : items) {
        PluginState state = item.completed;
        if (!running) {
            state = item.plugin->saveState();
        } else {
            state.controlPortValues.clear();
            for (const auto& port : item.info.ports) {
                if (port.isInput && port.isControl && !port.isReadOnly) {
                    state.controlPortValues.emplace_back(
                            port.index, item.plugin->getParameter(port.index));
                }
            }
        }
        if (state.pluginUri.empty()) state.pluginUri = item.info.id;
        if (state.format.empty()) state.format = item.info.format;
        state.manualLatencyFrames = item.manualLatencyFrames;
        chain.plugins.push_back(state);

        if (!running) {
            state.manualLatencyFrames = 0;
            std::lock_guard lock(controlMutex_);
            for (auto& slot : plugins_) {
                if (slot.instanceId == item.instanceId && slot.plugin == item.plugin) {
                    slot.completedState = std::move(state);
                    break;
                }
            }
        }
    }
    return chain;
}

bool PluginChain::restorePluginState(int index, const PluginState& state) {
    if (state.manualLatencyFrames > kMaxSupportedPdcFrames) return false;

    std::shared_ptr<IPlugin> plugin;
    uint64_t instanceId = 0;
    uint32_t previousManualLatency = 0;
    {
        std::lock_guard lock(controlMutex_);
        if (running_ || deactivationPending_ || index < 0 ||
            index >= static_cast<int>(plugins_.size())) {
            return false;
        }
        const auto& slot = plugins_[static_cast<size_t>(index)];
        plugin = slot.plugin;
        instanceId = slot.instanceId;
        previousManualLatency = slot.manualLatencyFrames;
    }

    PluginState previousState;
    try {
        previousState = plugin->saveState();
    } catch (...) {
        return false;
    }
    PluginState pluginState = state;
    pluginState.manualLatencyFrames = 0;
    if (!plugin->restoreState(pluginState)) return false;

    bool rollback = false;
    {
        std::lock_guard lock(controlMutex_);
        auto slot = std::find_if(
                plugins_.begin(), plugins_.end(), [&](const PluginSlot& candidate) {
                    return candidate.instanceId == instanceId && candidate.plugin == plugin;
                });
        if (slot == plugins_.end()) {
            rollback = true;
        } else {
            slot->manualLatencyFrames = state.manualLatencyFrames;
            recomputeLatencyLocked();
            rollback = latencyOverflow_.load(std::memory_order_relaxed);
            if (!rollback && planPublished_) rollback = !rebuildPlanLocked();
            if (rollback) {
                slot->manualLatencyFrames = previousManualLatency;
            } else {
                slot->completedState = pluginState;
                ++generation_;
            }
        }
    }
    if (rollback) {
        (void)plugin->restoreState(previousState);
        std::lock_guard lock(controlMutex_);
        recomputeLatencyLocked();
        return false;
    }
    return true;
}

void PluginChain::recomputeLatencyLocked() noexcept {
    uint64_t total = 0;
    for (const auto& slot : plugins_) {
        total += slot.plugin->getLatencyFrames();
        total += slot.manualLatencyFrames;
    }
    latencyOverflow_.store(total > kMaxSupportedPdcFrames, std::memory_order_relaxed);
    latencyFrames_.store(static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX)),
                         std::memory_order_relaxed);
}

void PluginChain::ensureBuffers(uint32_t frames, uint32_t channels) {
    if (intermediateBuffers_.size() < channels) intermediateBuffers_.resize(channels);
    for (auto& buffer : intermediateBuffers_) {
        if (buffer.size() < frames) buffer.resize(frames);
    }
}

void PluginChain::clearOutputs(float* const* outputs, uint32_t frames) const noexcept {
    if (!outputs) return;
    for (uint32_t channel = 0; channel < 2; ++channel) {
        if (outputs[channel]) std::memset(outputs[channel], 0, frames * sizeof(float));
    }
}

} // namespace guitarrackcraft
