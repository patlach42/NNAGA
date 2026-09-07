#pragma once

#include <array>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace vsthost {

struct VstInstancePaths {
    std::string token;
    std::string shmPath;
    std::string pickerPath;
    std::string logSuffix;
    int shmFd = -1;
    int pickerFd = -1;

    VstInstancePaths() = default;
    VstInstancePaths(const VstInstancePaths&) = delete;
    VstInstancePaths& operator=(const VstInstancePaths&) = delete;
    VstInstancePaths(VstInstancePaths&& other) noexcept
        : token(std::move(other.token)), shmPath(std::move(other.shmPath)),
          pickerPath(std::move(other.pickerPath)), logSuffix(std::move(other.logSuffix)),
          shmFd(std::exchange(other.shmFd, -1)), pickerFd(std::exchange(other.pickerFd, -1)) {}
    VstInstancePaths& operator=(VstInstancePaths&& other) noexcept {
        if (this != &other) {
            cleanup();
            token = std::move(other.token); shmPath = std::move(other.shmPath);
            pickerPath = std::move(other.pickerPath); logSuffix = std::move(other.logSuffix);
            shmFd = std::exchange(other.shmFd, -1);
            pickerFd = std::exchange(other.pickerFd, -1);
        }
        return *this;
    }
    ~VstInstancePaths() { cleanup(); }
    int releaseShmFd() noexcept { return std::exchange(shmFd, -1); }
    int releasePickerFd() noexcept { return std::exchange(pickerFd, -1); }
    void retainPostmortem() noexcept {
        if (pickerFd >= 0) ::close(std::exchange(pickerFd, -1));
        if (!pickerPath.empty()) ::unlink(pickerPath.c_str());
        if (shmFd >= 0) ::close(std::exchange(shmFd, -1));
        if (!shmPath.empty()) {
            const size_t marker = shmPath.find("_i", shmPath.find_last_of('/') + 1);
            const std::string legacy = marker == std::string::npos
                ? shmPath + ".dat" : shmPath.substr(0, marker) + ".dat";
            if (::rename(shmPath.c_str(), legacy.c_str()) != 0) ::unlink(shmPath.c_str());
            shmPath.clear();
        }
    }
private:
    void cleanup() noexcept {
        if (shmFd >= 0) ::close(std::exchange(shmFd, -1));
        if (pickerFd >= 0) ::close(std::exchange(pickerFd, -1));
        if (!shmPath.empty()) ::unlink(shmPath.c_str());
        if (!pickerPath.empty()) ::unlink(pickerPath.c_str());
    }
};

inline bool isCanonicalUuid(const std::string& uuid) {
    static constexpr std::array<int, 5> lengths{{8, 4, 4, 4, 12}};
    size_t pos = 0;
    for (size_t part = 0; part < lengths.size(); ++part) {
        if (part != 0) { if (pos >= uuid.size() || uuid[pos++] != '-') return false; }
        for (int i = 0; i < lengths[part]; ++i)
            if (pos >= uuid.size() || !std::isxdigit(static_cast<unsigned char>(uuid[pos++]))) return false;
    }
    return pos == uuid.size();
}

inline std::optional<VstInstancePaths> reserveVstInstancePaths(
        const std::string& tmpDir, const std::string& uuid) {
    if (tmpDir.empty() || !isCanonicalUuid(uuid)) return std::nullopt;
    const std::string prefix = tmpDir + "/vst_shm_v" + uuid + "_i";
    std::string templ = prefix + "XXXXXX";
    if (templ.size() + std::string(".wake").size() >= 108) return std::nullopt;
    std::vector<char> chars(templ.begin(), templ.end()); chars.push_back('\0');
    const int fd = ::mkstemp(chars.data());
    if (fd < 0) return std::nullopt;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        ::close(fd);
        ::unlink(chars.data());
        return std::nullopt;
    }
    VstInstancePaths result;
    result.shmFd = fd;
    result.shmPath = chars.data();
    result.token = result.shmPath.substr(prefix.size() - 2);
    result.pickerPath = tmpDir + "/vst_picker_v" + uuid + result.token + ".dat";
    result.logSuffix = "v" + uuid + result.token;
    result.pickerFd = ::open(
        result.pickerPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (result.pickerFd < 0) { result.pickerPath.clear(); return std::nullopt; }
    if (result.pickerPath.size() >= 4096 || result.shmPath.size() + 5 >= 108) return std::nullopt;
    return result;
}

} // namespace vsthost
