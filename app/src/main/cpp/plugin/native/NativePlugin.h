#ifndef GUITARRACKCRAFT_NATIVE_PLUGIN_ADAPTER_H
#define GUITARRACKCRAFT_NATIVE_PLUGIN_ADAPTER_H

#include "../IPlugin.h"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nnaga/native_plugin.h>

namespace guitarrackcraft {

struct NativePluginLibrary {
    void* handle = nullptr;
    const NnagaPluginLibraryV1* abi = nullptr;
    std::string path;
    ~NativePluginLibrary();
};

bool validateNativePluginLibrary(const std::shared_ptr<NativePluginLibrary>& library,
                                 std::vector<const NnagaPluginDescriptorV1*>* descriptors,
                                 std::string* error);

class NativePlugin final : public IPlugin {
public:
    NativePlugin(std::shared_ptr<NativePluginLibrary> library, const NnagaPluginDescriptorV1* descriptor);
    ~NativePlugin() override;
    void activate(float sampleRate, uint32_t bufferSize) override;
    void deactivate() override;
    uint32_t process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                     const AudioProcessContext& context, const MidiEvent* inputEvents, uint32_t inputCount,
                     MidiEvent* outputEvents, uint32_t outputCapacity) override;
    PluginInfo getInfo() const override { return info_; }
    uint32_t getLatencyFrames() const noexcept override;
    void setParameter(uint32_t portIndex, float value) override;
    float getParameter(uint32_t portIndex) const override;
    std::string getParameterDisplay(uint32_t portIndex) const override;
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
    PluginState saveState() override;
    bool restoreState(const PluginState& state) override;
private:
    uint32_t ordinalForPort(uint32_t portIndex) const noexcept;
    std::shared_ptr<NativePluginLibrary> library_;
    const NnagaPluginDescriptorV1* descriptor_;
    NnagaPluginHandle handle_ = nullptr;
    PluginInfo info_;
    std::array<std::atomic<float>, NNAGA_NATIVE_MAX_PARAMETERS> values_{};
    std::array<std::atomic<uint64_t>, 4> dirty_{};
    std::array<uint32_t, NNAGA_NATIVE_MAX_PARAMETERS> ports_{};
    uint32_t parameterCount_ = 0;
};
} // namespace guitarrackcraft
#endif
