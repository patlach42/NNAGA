/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "JsfxPluginFactory.h"
#include "JsfxPlugin.h"
#include <algorithm>
#include <fstream>
namespace guitarrackcraft {
namespace {
constexpr std::size_t kDescriptorScanLines = 32;

bool hasJsfxDescription(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    for (std::size_t i = 0; i < kDescriptorScanLines && std::getline(file, line); ++i) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line.compare(first, 5, "desc:") == 0) return true;
    }
    return false;
}

bool isJsfxMain(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    if (path.extension() == ".jsfx") return true;
    return path.extension().empty() && hasJsfxDescription(path);
}
}
JsfxPluginFactory::JsfxPluginFactory(std::string effectsRoot, std::string dataRoot): effectsRoot_(std::move(effectsRoot)), dataRoot_(std::move(dataRoot)) {}
bool JsfxPluginFactory::initialize() {
    scripts_.clear(); std::error_code ec; if (!std::filesystem::is_directory(effectsRoot_, ec)) return false;
    const auto root=std::filesystem::weakly_canonical(effectsRoot_,ec); if(ec) return false;
    for(std::filesystem::recursive_directory_iterator it(root,ec),end;it!=end&&!ec;it.increment(ec)) { if(!isJsfxMain(it->path())) continue; auto p=std::filesystem::weakly_canonical(it->path(),ec); if(ec) continue; auto rel=p.lexically_relative(root); if(rel.empty()||rel.is_absolute()||rel.string().find("..") == 0) continue; scripts_.push_back(std::move(p)); }
    std::sort(scripts_.begin(),scripts_.end()); config_=std::shared_ptr<ysfx_config_t>(ysfx_config_new(),ysfx_config_free); if(!config_) return false;
    ysfx_set_import_root(config_.get(),root.string().c_str()); ysfx_set_data_root(config_.get(),dataRoot_.string().c_str()); ysfx_register_builtin_audio_formats(config_.get()); return true;
}
std::vector<PluginInfo> JsfxPluginFactory::enumeratePlugins() { std::vector<PluginInfo> result; const auto root=std::filesystem::weakly_canonical(effectsRoot_); for(const auto&p:scripts_) { auto id=p.lexically_relative(root).generic_string(); auto plugin=std::make_unique<JsfxPlugin>(config_,p.string(),id); if(plugin->loaded()){result.push_back(plugin->getInfo());}} return result; }
std::unique_ptr<IPlugin> JsfxPluginFactory::createPlugin(const std::string&id) { if(id.empty()||id.find("..")!=std::string::npos||id.front()=='/') return nullptr; std::error_code ec; const auto root=std::filesystem::weakly_canonical(effectsRoot_,ec); const auto p=std::filesystem::weakly_canonical(root/std::filesystem::path(id),ec); if(ec||p.lexically_relative(root).empty()||p.lexically_relative(root).string().find("..") == 0||!isJsfxMain(p)) return nullptr; if(std::find(scripts_.begin(),scripts_.end(),p)==scripts_.end()) return nullptr; auto plugin=std::make_unique<JsfxPlugin>(config_,p.string(),id); return plugin->loaded()?std::move(plugin):nullptr; }
}
