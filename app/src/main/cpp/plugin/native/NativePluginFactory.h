#ifndef NNAGA_NATIVE_PLUGIN_FACTORY_H
#define NNAGA_NATIVE_PLUGIN_FACTORY_H

#include "../IPluginFactory.h"
#include "NativePlugin.h"
#include <string>
#include <unordered_map>

namespace guitarrackcraft {
class NativePluginFactory final : public IPluginFactory {
public:
    NativePluginFactory(std::string filesDir, std::string nativeLibDir, std::string pluginLibDir);
    std::string getFormat() const override { return "NATIVE"; }
    std::vector<PluginInfo> enumeratePlugins() override;
    std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) override;
    bool initialize() override;
private:
    std::string filesDir_;
    std::string nativeLibDir_;
    std::string pluginLibDir_;
    std::vector<PluginInfo> plugins_;
    std::unordered_map<std::string, std::pair<std::shared_ptr<NativePluginLibrary>, const NnagaPluginDescriptorV1*>> descriptors_;
};

bool validateNativePluginPath(const std::string& path);
} // namespace guitarrackcraft
#endif
