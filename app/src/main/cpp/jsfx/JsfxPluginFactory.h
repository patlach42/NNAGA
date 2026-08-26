/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once
#include "../plugin/IPluginFactory.h"
#include <filesystem>
#include <memory>
#include <ysfx.h>
namespace guitarrackcraft {
class JsfxPluginFactory final : public IPluginFactory {
public:
    explicit JsfxPluginFactory(std::string effectsRoot, std::string dataRoot);
    std::string getFormat() const override { return "JSFX"; }
    std::vector<PluginInfo> enumeratePlugins() override;
    std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) override;
    bool initialize() override;
private:
    std::filesystem::path effectsRoot_, dataRoot_;
    std::shared_ptr<ysfx_config_t> config_;
    std::vector<std::filesystem::path> scripts_;
};
}
