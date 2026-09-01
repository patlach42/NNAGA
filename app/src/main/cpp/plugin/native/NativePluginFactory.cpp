#include "NativePluginFactory.h"

#include <android/dlext.h>
#include <android/log.h>
#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <regex>
#include <set>

#include <unistd.h>
namespace fs = std::filesystem;
namespace guitarrackcraft {
namespace {
constexpr char kTag[] = "NativePluginFactory";
const std::regex kLibraryName("libnnaga_plugin_[A-Za-z0-9_.-]+\\.so");

std::vector<std::string> candidates(const std::string& root) {
    std::vector<std::string> paths;
    if (root.empty() || !fs::is_directory(root)) return paths;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
        if (error) { error.clear(); continue; }
        const auto& entry = *it;
        if (entry.is_symlink(error) || !entry.is_regular_file(error) || !std::regex_match(entry.path().filename().string(), kLibraryName)) continue;
        const fs::path canonical = fs::weakly_canonical(entry.path(), error);
        if (!error) paths.push_back(canonical.string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void* openHandle(const std::string& path) {
#if defined(__ANDROID__)
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;
    void* handle = android_dlopen_ext(path.c_str(), RTLD_NOW | RTLD_LOCAL, &ext);
    close(fd);
    return handle;
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

std::shared_ptr<NativePluginLibrary> openLibrary(const std::string& path) {
    void* handle = openHandle(path);
    if (!handle) return {};
    auto entry = reinterpret_cast<const NnagaPluginLibraryV2* (*)(uint32_t) noexcept>(
        dlsym(handle, "nnaga_plugin_entry"));
    if (!entry) { dlclose(handle); return {}; }
    const auto* abi = entry(NNAGA_NATIVE_ABI_VERSION);
    if (!abi) { dlclose(handle); return {}; }
    auto library = std::make_shared<NativePluginLibrary>();
    library->handle = handle;
    library->abi = abi;
    library->path = path;
    return library;
}
} // namespace

NativePluginFactory::NativePluginFactory(std::string filesDir, std::string nativeLibDir, std::string pluginLibDir)
    : filesDir_(std::move(filesDir)), nativeLibDir_(std::move(nativeLibDir)), pluginLibDir_(std::move(pluginLibDir)) {}

bool NativePluginFactory::initialize() {
    plugins_.clear();
    descriptors_.clear();
    const std::vector<std::string> roots = {filesDir_ + "/plugin-repositories/installed/native", nativeLibDir_, pluginLibDir_};
    for (const auto& root : roots) for (const auto& path : candidates(root)) {
        auto library = openLibrary(path);
        std::vector<const NnagaPluginDescriptorV2*> descriptors;
        std::string error;
        if (!validateNativePluginLibrary(library, &descriptors, &error)) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "Skipping %s: %s", path.c_str(), error.c_str());
            continue;
        }
        for (const auto* descriptor : descriptors) {
            if (descriptors_.count(descriptor->id)) {
                __android_log_print(ANDROID_LOG_WARN, kTag, "Skipping duplicate native plugin %s", descriptor->id);
                continue;
            }
            NativePlugin preview(library, descriptor);
            if (preview.getInfo().id.empty()) continue;
            plugins_.push_back(preview.getInfo());
            descriptors_.emplace(descriptor->id, std::make_pair(library, descriptor));
        }
    }
    return true;
}

std::vector<PluginInfo> NativePluginFactory::enumeratePlugins() { return plugins_; }

std::unique_ptr<IPlugin> NativePluginFactory::createPlugin(const std::string& pluginId) {
    const auto it = descriptors_.find(pluginId);
    if (it == descriptors_.end()) return nullptr;
    auto plugin = std::make_unique<NativePlugin>(it->second.first, it->second.second);
    if (plugin->getInfo().id.empty()) return nullptr;
    return plugin;
}

bool validateNativePluginPath(const std::string& path) {
    auto library = openLibrary(path);
    return validateNativePluginLibrary(library, nullptr, nullptr);
}
} // namespace guitarrackcraft
