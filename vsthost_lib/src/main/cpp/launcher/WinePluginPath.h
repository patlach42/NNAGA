#pragma once

#include <optional>
#include <string>

namespace vsthost {

/**
 * Translate a plugin already installed below a Wine prefix's drive_c into its
 * native C:\\ path. Keeping that path preserves the module's original sibling
 * resources and preset directories while still avoiding Wine's sandboxed Z:
 * volume.
 *
 * Returns nullopt for plugins outside the prefix; callers may stage those onto
 * drive_c using their existing fallback.
 */
inline std::optional<std::string> prefixLocalPluginWindowsPath(
    const std::string& winePrefix,
    const std::string& pluginPath) {
    if (winePrefix.empty() || pluginPath.empty() ||
        winePrefix.front() != '/' || pluginPath.front() != '/') {
        return std::nullopt;
    }

    std::string normalizedPrefix = winePrefix;
    while (normalizedPrefix.size() > 1 && normalizedPrefix.back() == '/') {
        normalizedPrefix.pop_back();
    }

    const std::string driveRoot = normalizedPrefix + "/drive_c/";
    if (pluginPath.compare(0, driveRoot.size(), driveRoot) != 0 ||
        pluginPath.size() == driveRoot.size()) {
        return std::nullopt;
    }

    std::string windowsPath = "C:\\";
    windowsPath.reserve(3 + pluginPath.size() - driveRoot.size());
    for (size_t i = driveRoot.size(); i < pluginPath.size(); ++i) {
        windowsPath.push_back(pluginPath[i] == '/' ? '\\' : pluginPath[i]);
    }
    return windowsPath;
}

}  // namespace vsthost
