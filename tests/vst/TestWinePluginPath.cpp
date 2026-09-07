#include <gtest/gtest.h>

#include "ipc/VstInstancePaths.h"
#include "ipc/PickerChannel.h"
#include "ipc/SharedRing.h"
#include "launcher/WinePluginPath.h"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using vsthost::prefixLocalPluginWindowsPath;
using vsthost::reserveVstInstancePaths;

class VstInstancePathsTest : public ::testing::Test {
 protected:
    void SetUp() override {
        testDirectory_ = std::filesystem::temp_directory_path();
        testDirectory_ /= "nnaga-vst-instance-path-" + std::to_string(getpid());

        std::error_code error;
        std::filesystem::remove_all(testDirectory_, error);
        ASSERT_FALSE(error) << "remove stale test directory: " << error.message();
        ASSERT_TRUE(std::filesystem::create_directories(testDirectory_, error) ||
                    std::filesystem::is_directory(testDirectory_));
        ASSERT_FALSE(error) << "create test directory: " << error.message();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(testDirectory_, error);
        EXPECT_FALSE(error) << "remove test directory: " << error.message();
    }

    std::filesystem::path testDirectory_;
};

constexpr char kUuid[] = "01234567-89ab-cdef-0123-456789abcdef";
TEST_F(VstInstancePathsTest, ReservationsForSameUuidAreFullyDistinct) {
    auto first = reserveVstInstancePaths(testDirectory_.string(), kUuid);
    auto second = reserveVstInstancePaths(testDirectory_.string(), kUuid);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(first->token, second->token);
    EXPECT_NE(first->shmPath, second->shmPath);
    EXPECT_NE(first->pickerPath, second->pickerPath);
    EXPECT_NE(first->logSuffix, second->logSuffix);
    EXPECT_NE(first->shmPath + ".wake", second->shmPath + ".wake");

    for (const auto* reservation : {&*first, &*second}) {
        SCOPED_TRACE(reservation->token);
        EXPECT_NE(reservation->token, "");
        EXPECT_NE(reservation->shmPath.find(reservation->token), std::string::npos);
        EXPECT_NE(reservation->pickerPath.find(reservation->token), std::string::npos);
        EXPECT_NE(reservation->logSuffix.find(reservation->token), std::string::npos);
        EXPECT_NE(reservation->shmPath.find(kUuid), std::string::npos);
        EXPECT_GE(reservation->shmFd, 0);
        EXPECT_GE(reservation->pickerFd, 0);

        std::error_code error;
        EXPECT_TRUE(std::filesystem::is_regular_file(reservation->shmPath, error));
        EXPECT_FALSE(error) << "inspect reserved shm file: " << error.message();
        error.clear();
        EXPECT_TRUE(std::filesystem::is_regular_file(reservation->pickerPath, error));
        EXPECT_FALSE(error) << "inspect reserved picker file: " << error.message();
    }
}


TEST_F(VstInstancePathsTest, ReservedShmFdCannotBeRedirectedByPathReplacement) {
    auto reservation = reserveVstInstancePaths(testDirectory_.string(), kUuid);
    ASSERT_TRUE(reservation.has_value());

    const std::string originalPath = reservation->shmPath + ".original";
    const std::string decoyPath = reservation->shmPath + ".decoy";
    ASSERT_EQ(::rename(reservation->shmPath.c_str(), originalPath.c_str()), 0);
    const int decoyFd =
        ::open(decoyPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(decoyFd, 0);
    ASSERT_EQ(::close(decoyFd), 0);
    ASSERT_EQ(::symlink(decoyPath.c_str(), reservation->shmPath.c_str()), 0);

    const int reservedFd = reservation->releaseShmFd();
    ASSERT_GE(reservedFd, 0);
    SharedRing ring(reservation->shmPath, reservedFd);
    ASSERT_TRUE(ring.valid());

    constexpr uint64_t marker = 0xdecafbad12345678ULL;
    __atomic_store_n(&ring.raw()->guest_frames_produced, marker,
                     __ATOMIC_RELEASE);

    const int originalFd = ::open(originalPath.c_str(), O_RDONLY);
    ASSERT_GE(originalFd, 0);
    uint64_t originalValue = 0;
    ASSERT_EQ(::pread(originalFd, &originalValue, sizeof(originalValue),
                      offsetof(VstpocShared, guest_frames_produced)),
              static_cast<ssize_t>(sizeof(originalValue)));
    ::close(originalFd);
    EXPECT_EQ(originalValue, marker);

    const int redirectedFd = ::open(decoyPath.c_str(), O_RDONLY);
    ASSERT_GE(redirectedFd, 0);
    uint64_t redirectedValue = 0;
    const ssize_t redirectedBytes =
        ::pread(redirectedFd, &redirectedValue, sizeof(redirectedValue),
                offsetof(VstpocShared, guest_frames_produced));
    ::close(redirectedFd);
    EXPECT_NE(redirectedBytes, static_cast<ssize_t>(sizeof(redirectedValue)));
}

TEST_F(VstInstancePathsTest, ReservedPickerFdCannotBeRedirectedByPathReplacement) {
    auto reservation = reserveVstInstancePaths(testDirectory_.string(), kUuid);
    ASSERT_TRUE(reservation.has_value());

    const std::string originalPath = reservation->pickerPath + ".original";
    const std::string decoyPath = reservation->pickerPath + ".decoy";
    ASSERT_EQ(::rename(reservation->pickerPath.c_str(), originalPath.c_str()), 0);
    const int decoyFd =
        ::open(decoyPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(decoyFd, 0);

    ASSERT_EQ(::close(decoyFd), 0);
    ASSERT_EQ(::symlink(decoyPath.c_str(), reservation->pickerPath.c_str()), 0);

    const int reservedFd = reservation->releasePickerFd();
    ASSERT_GE(reservedFd, 0);
    PickerChannel picker(reservation->pickerPath, reservedFd);
    ASSERT_TRUE(picker.valid());

    const int originalFd = ::open(originalPath.c_str(), O_RDWR);
    ASSERT_GE(originalFd, 0);
    void* mapped = ::mmap(nullptr, sizeof(VstpocPickerChannel),
                           PROT_READ | PROT_WRITE, MAP_SHARED, originalFd, 0);
    ASSERT_NE(mapped, MAP_FAILED);
    auto* shared = static_cast<VstpocPickerChannel*>(mapped);
    __atomic_store_n(&shared->response_seq, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&shared->request_seq, 17u, __ATOMIC_RELEASE);
    ASSERT_EQ(::munmap(mapped, sizeof(VstpocPickerChannel)), 0);
    ASSERT_EQ(::close(originalFd), 0);

    uint32_t requestSeq = 0;
    EXPECT_TRUE(picker.hasRequest(&requestSeq));
    EXPECT_EQ(requestSeq, 17u);
}
TEST_F(VstInstancePathsTest, ReservationDestructorRemovesUniqueFiles) {
    std::string shmPath;
    std::string pickerPath;
    {
        auto reservation = reserveVstInstancePaths(testDirectory_.string(), kUuid);
        ASSERT_TRUE(reservation.has_value());
        shmPath = reservation->shmPath;
        pickerPath = reservation->pickerPath;
        ASSERT_TRUE(std::filesystem::exists(shmPath));
        ASSERT_TRUE(std::filesystem::exists(pickerPath));
    }
    EXPECT_FALSE(std::filesystem::exists(shmPath));
    EXPECT_FALSE(std::filesystem::exists(pickerPath));
}

TEST_F(VstInstancePathsTest, RetainPostmortemRemovesPickerAndKeepsLegacyShm) {
    auto reservation = reserveVstInstancePaths(testDirectory_.string(), kUuid);
    ASSERT_TRUE(reservation.has_value());
    const std::string uniqueShm = reservation->shmPath;
    const std::string picker = reservation->pickerPath;
    const std::size_t marker = uniqueShm.find("_i", uniqueShm.find_last_of('/'));
    ASSERT_NE(marker, std::string::npos);
    const std::string legacyShm = uniqueShm.substr(0, marker) + ".dat";

    reservation->retainPostmortem();

    EXPECT_FALSE(std::filesystem::exists(uniqueShm));
    EXPECT_FALSE(std::filesystem::exists(picker));
    std::error_code error;
    EXPECT_TRUE(std::filesystem::is_regular_file(legacyShm, error));
    EXPECT_FALSE(error) << "inspect retained legacy shm: " << error.message();
}
TEST_F(VstInstancePathsTest, LongAndroidLikeBaseStaysWithinWakePathBoundOrRejects) {
    auto longDirectory = testDirectory_;
    longDirectory /=
        "data/user/0/com.example.nnaga.instrument.host.with.a.long.package/cache";
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(longDirectory, error));
    ASSERT_FALSE(error) << "create long test directory: " << error.message();

    const auto reservation = reserveVstInstancePaths(longDirectory.string(), kUuid);
    if (!reservation.has_value()) {
        return;
    }

    EXPECT_LE(reservation->shmPath.size() + std::string(".wake").size(), 107U);
}

TEST_F(VstInstancePathsTest, UnsafeOrOverlongInputsRejectWithoutTruncatingOrColliding) {
    const std::string longUuidA = std::string(kUuid) + std::string(180, 'a');
    const std::string longUuidB = std::string(kUuid) + std::string(179, 'a') + "b";

    EXPECT_FALSE(reserveVstInstancePaths(testDirectory_.string(), "uuid/escape"));
    EXPECT_FALSE(reserveVstInstancePaths(testDirectory_.string(), "not-a-canonical-uuid"));
    EXPECT_FALSE(reserveVstInstancePaths(testDirectory_.string(), longUuidA));
    EXPECT_FALSE(reserveVstInstancePaths(testDirectory_.string(), longUuidB));

    auto tooLongDirectory = testDirectory_;
    tooLongDirectory /= std::string(180, 'p');
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(tooLongDirectory, error));
    ASSERT_FALSE(error) << "create overlong test directory: " << error.message();
    EXPECT_FALSE(reserveVstInstancePaths(tooLongDirectory.string(), kUuid));
}

TEST(WinePluginPathTest, ConvertsPluginPathWithSpacesToWindowsPath) {
    const auto result = prefixLocalPluginWindowsPath(
        "/tmp/wine prefix",
        "/tmp/wine prefix/drive_c/Program Files/VST Plugins/My Synth.dll");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "C:\\Program Files\\VST Plugins\\My Synth.dll");
}

TEST(WinePluginPathTest, ConvertsVst3BundlePathToWindowsPath) {
    const auto result = prefixLocalPluginWindowsPath(
        "/opt/vst/wineprefix",
        "/opt/vst/wineprefix/drive_c/Program Files/Common Files/VST3/Example.vst3/Contents/x86_64-win/Example.vst3");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "C:\\Program Files\\Common Files\\VST3\\Example.vst3\\Contents\\x86_64-win\\Example.vst3");
}

TEST(WinePluginPathTest, AcceptsPrefixWithTrailingSlash) {
    const auto result = prefixLocalPluginWindowsPath(
        "/tmp/wineprefix/",
        "/tmp/wineprefix/drive_c/VST3/Plugin.vst3/Contents/Plugin.vst3");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "C:\\VST3\\Plugin.vst3\\Contents\\Plugin.vst3");
}

TEST(WinePluginPathTest, RejectsPathsOutsidePrefixDriveC) {
    struct RejectionCase {
        const char* name;
        const char* winePrefix;
        const char* pluginPath;
    };
    const std::vector<RejectionCase> cases{
        {"externalPrefix", "/tmp/wineprefix", "/tmp/otherprefix/drive_c/VST/Plugin.dll"},
        {"sharedPrefixName", "/tmp/wineprefix_v1", "/tmp/wineprefix_v10/drive_c/VST/Plugin.dll"},
        {"driveCRoot", "/tmp/wineprefix", "/tmp/wineprefix/drive_c"},
        {"driveCRootTrailingSlash", "/tmp/wineprefix", "/tmp/wineprefix/drive_c/"},
        {"relativePluginPath", "/tmp/wineprefix", "wineprefix/drive_c/VST/Plugin.dll"},
        {"relativePrefix", "wineprefix", "/tmp/wineprefix/drive_c/VST/Plugin.dll"},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        EXPECT_FALSE(prefixLocalPluginWindowsPath(testCase.winePrefix, testCase.pluginPath).has_value());
    }
}

}  // namespace
