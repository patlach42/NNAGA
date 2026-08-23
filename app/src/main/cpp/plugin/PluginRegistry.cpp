/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#include "PluginRegistry.h"
#include <algorithm>

namespace guitarrackcraft {

void PluginRegistry::registerFactory(std::unique_ptr<IPluginFactory> factory) {
    if (factory) {
        std::unique_lock lock(mutex_);
        factories_.push_back(std::move(factory));
    }
}

bool PluginRegistry::initializeAll() {
    std::unique_lock lock(mutex_);
    pluginCache_.clear();
    bool allSucceeded = true;
    for (auto& factory : factories_) {
        if (!factory->initialize()) { allSucceeded = false; continue; }
        for (const auto& plugin : factory->enumeratePlugins()) {
            const std::string& fmt = plugin.format.empty() ? factory->getFormat() : plugin.format;
            pluginCache_[fmt + ":" + plugin.id] = plugin;
        }
    }
    return allSucceeded;
}

std::vector<PluginInfo> PluginRegistry::getAllPlugins() const {
    std::shared_lock lock(mutex_);
    std::vector<PluginInfo> allPlugins;
    allPlugins.reserve(pluginCache_.size());
    for (const auto& pair : pluginCache_) allPlugins.push_back(pair.second);
    return allPlugins;
}

std::unique_ptr<IPlugin> PluginRegistry::createPlugin(const std::string& pluginId) const {
    std::shared_lock lock(mutex_);
    const size_t colonPos = pluginId.find(':');
    if (colonPos == std::string::npos) return nullptr;
    const std::string format = pluginId.substr(0, colonPos);
    const std::string id = pluginId.substr(colonPos + 1);
    for (const auto& factory : factories_) if (factory->acceptsFormat(format)) return factory->createPlugin(id);
    return nullptr;
}

PluginInfo PluginRegistry::getPluginInfo(const std::string& pluginId) const {
    std::shared_lock lock(mutex_);
    auto it = pluginCache_.find(pluginId);
    return it == pluginCache_.end() ? PluginInfo{} : it->second;
}
} // namespace guitarrackcraft
