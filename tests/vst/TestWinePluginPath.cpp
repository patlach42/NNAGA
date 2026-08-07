#include <gtest/gtest.h>

#include "launcher/WinePluginPath.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using vsthost::prefixLocalPluginWindowsPath;

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
