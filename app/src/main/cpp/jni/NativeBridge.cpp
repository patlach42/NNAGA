/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <signal.h>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include "../engine/AudioEngine.h"
#include "../plugin/PluginUIGuard.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/IPlugin.h"
#include "../plugin/IPluginFactory.h"
#include "../plugin/lv2/LV2PluginFactory.h"

// VST hosting (full flavor only — gated by CMake -DHAS_VST_HOST from
// productFlavors.full.externalNativeBuild). Header comes from :vsthost_lib's
// prefab package (vsthost_lib::vsthost target → -I<prefab>/include/vsthost/).
#if HAS_VST_HOST
// Prefab adds vsthost_lib/src/main/cpp to the include path as -isystem.
#include <vst/VstFactory.h>
#endif
#include "../plugin/PluginUIManager.h"
#include "../x11/X11NativeDisplay.h"
#include "../x11/X11Worker.h"
#include "../x11/DisplayState.h"
#include "../utils/ThreadUtils.h"

#define LOG_TAG "NativeBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace guitarrackcraft;

struct NativeContext {
    // Destruction order matters: AudioEngine holds a non-owning pointer to
    // directUsbOutput_, so it must be destroyed first.
    std::unique_ptr<DirectUsbOutput> directUsbOutput;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<PluginRegistry> pluginRegistry;
    std::unique_ptr<PluginUIManager> pluginUIManager;
    std::mutex rackControlMutex;
    std::string lv2Path;
    std::string nativeLibDir;
    std::string filesDir;
    std::string x11LibsDir;
    std::string pluginLibDir;  // PAD-extracted plugin .so (playstore flavor)
};

static NativeContext* g_ctx = nullptr;

// NativeContext is process-local and must be published exactly once.  The
// context owns the live audio graph, so replacing it during Activity
// recreation would tear down the running engine.
static std::once_flag g_ctxOnce;

static NativeContext* ensureCtx() {
    std::call_once(g_ctxOnce, [] {
        g_ctx = new NativeContext();
    });
    return g_ctx;
}

// Serialize initialization independently of the JNI caller threads.  This
// also makes the initialized-engine fast path atomic with the first init.
static std::mutex g_nativeInitMutex;

// Cached JNI class/method references (initialized lazily)
static struct {
    jclass pluginInfoClass = nullptr;
    jmethodID pluginInfoCtor = nullptr;
    jfieldID piId = nullptr;
    jfieldID piName = nullptr;
    jfieldID piFormat = nullptr;
    jfieldID piPorts = nullptr;
    jfieldID piModguiBasePath = nullptr;
    jfieldID piModguiIconTemplate = nullptr;
    jfieldID piHasX11Ui = nullptr;
    jfieldID piX11UiBinaryPath = nullptr;
    jfieldID piX11UiUri = nullptr;

    jclass portInfoClass = nullptr;
    jmethodID portInfoCtor = nullptr;

    jclass scalePointClass = nullptr;
    jmethodID scalePointCtor = nullptr;

    jclass arrayListClass = nullptr;
    jmethodID arrayListCtor = nullptr;
    jmethodID arrayListAdd = nullptr;
} g_jni;

static bool ensureJniCache(JNIEnv* env) {
    if (g_jni.pluginInfoClass) return true;

    auto cache = [&](const char* name) -> jclass {
        jclass local = env->FindClass(name);
        return local ? (jclass)env->NewGlobalRef(local) : nullptr;
    };

    g_jni.arrayListClass = cache("java/util/ArrayList");
    if (!g_jni.arrayListClass) return false;
    g_jni.arrayListCtor = env->GetMethodID(g_jni.arrayListClass, "<init>", "(I)V");
    g_jni.arrayListAdd = env->GetMethodID(g_jni.arrayListClass, "add", "(Ljava/lang/Object;)Z");

    g_jni.scalePointClass = cache("com/vibes/dsp/engine/ScalePoint");
    if (g_jni.scalePointClass)
        g_jni.scalePointCtor = env->GetMethodID(g_jni.scalePointClass, "<init>", "(Ljava/lang/String;F)V");

    g_jni.portInfoClass = cache("com/vibes/dsp/engine/PortInfo");
    if (!g_jni.portInfoClass) return false;
    g_jni.portInfoCtor = env->GetMethodID(g_jni.portInfoClass, "<init>",
        "(ILjava/lang/String;Ljava/lang/String;ZZZZFFFLjava/util/List;)V");

    g_jni.pluginInfoClass = cache("com/vibes/dsp/engine/PluginInfo");
    if (!g_jni.pluginInfoClass) return false;
    g_jni.pluginInfoCtor = env->GetMethodID(g_jni.pluginInfoClass, "<init>", "()V");
    g_jni.piId = env->GetFieldID(g_jni.pluginInfoClass, "id", "Ljava/lang/String;");
    g_jni.piName = env->GetFieldID(g_jni.pluginInfoClass, "name", "Ljava/lang/String;");
    g_jni.piFormat = env->GetFieldID(g_jni.pluginInfoClass, "format", "Ljava/lang/String;");
    g_jni.piPorts = env->GetFieldID(g_jni.pluginInfoClass, "ports", "Ljava/util/List;");
    g_jni.piModguiBasePath = env->GetFieldID(g_jni.pluginInfoClass, "modguiBasePath", "Ljava/lang/String;");
    g_jni.piModguiIconTemplate = env->GetFieldID(g_jni.pluginInfoClass, "modguiIconTemplate", "Ljava/lang/String;");
    g_jni.piHasX11Ui = env->GetFieldID(g_jni.pluginInfoClass, "hasX11Ui", "Z");
    g_jni.piX11UiBinaryPath = env->GetFieldID(g_jni.pluginInfoClass, "x11UiBinaryPath", "Ljava/lang/String;");
    g_jni.piX11UiUri = env->GetFieldID(g_jni.pluginInfoClass, "x11UiUri", "Ljava/lang/String;");

    return true;
}

namespace guitarrackcraft {

static DisplayState::Phase getDisplayPhase(int displayNumber) {
    std::lock_guard<std::mutex> lock(displayStateMutex());
    auto it = displayStates().find(displayNumber);
    if (it != displayStates().end()) {
        return it->second.phase;
    }
    return DisplayState::Phase::None;
}

// Update display phase
static void setDisplayPhase(int displayNumber, DisplayState::Phase phase) {
    std::lock_guard<std::mutex> lock(displayStateMutex());
    displayStates()[displayNumber].phase = phase;
    LOGI("Display %d phase -> %d", displayNumber, static_cast<int>(phase));
}

}  // namespace guitarrackcraft

// Minimal SIGABRT handler: log to stderr (async-signal-safe) then re-raise so tombstone is still generated.
static void sigabrt_handler(int signum) {
    (void)signum;
    const char msg[] = "GuitarRackCraft: SIGABRT received (check tombstone for backtrace)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;
    struct sigaction sa = {};
    sa.sa_handler = sigabrt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGABRT, &sa, nullptr);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeApplyCurrentThreadUiAffinity(
        JNIEnv*, jobject) {
#if defined(__linux__)
    applyCurrentThreadUiAffinity();
#endif
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetLv2Path(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->lv2Path.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->lv2Path = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("LV2 path set: %s", g_ctx->lv2Path.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetNativeLibDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->nativeLibDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->nativeLibDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Native lib dir set: %s", g_ctx->nativeLibDir.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetFilesDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->filesDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->filesDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Files dir set: %s", g_ctx->filesDir.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetX11LibsDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->x11LibsDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->x11LibsDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("X11 scratch dir set: %s", g_ctx->x11LibsDir.c_str());
        /* Preload X11 libs from nativeLibDir with RTLD_GLOBAL so plugin UI .so
         * can resolve DT_NEEDED (libxcb.so, libX11.so, etc.).
         * build.sh renames versioned files (libxcb.so.1 -> libxcb.so) so they're
         * extracted to nativeLibDir. Try unversioned first, then versioned as fallback. */
        std::string dir = g_ctx->nativeLibDir;
        if (!dir.empty() && dir.back() != '/') dir += '/';
        const char* libs[][2] = {
            {"libXau.so",  "libXau.so.6"},
            {"libxcb.so",  "libxcb.so.1"},
            {"libX11.so",  "libX11.so.6"},
            {nullptr, nullptr}
        };
        for (int i = 0; libs[i][0]; ++i) {
            std::string full = dir + libs[i][0];
            void* h = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (!h) {
                full = dir + libs[i][1];
                h = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
            }
            LOGI("X11 preload %s from nativeLibDir: %s", libs[i][0], h ? "ok" : "skip");
        }
    }
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetPluginLibDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->pluginLibDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->pluginLibDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Plugin lib dir set: %s", g_ctx->pluginLibDir.c_str());
    }
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeInit(JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> initLock(g_nativeInitMutex);
    NativeContext* ctx = ensureCtx();
    if (ctx->audioEngine && ctx->pluginRegistry && ctx->pluginUIManager) {
        LOGI("Native engine already initialized; preserving live state");
        return JNI_TRUE;
    }

    LOGI("Initializing native engine");

    // Promote libc++_shared.so to RTLD_GLOBAL so that LV2 plugin .so files
    // (which depend on it) can resolve the dependency when dlopen'd by lilv
    void* cxxLib = dlopen("libc++_shared.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (!cxxLib) {
        cxxLib = dlopen("libc++_shared.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!cxxLib) {
        LOGE("Warning: could not promote libc++_shared.so to global: %s", dlerror());
    }

    // NOTE: Do NOT call XInitThreads(). All Display* access is single-threaded via
    // the pluginUI thread in X11NativeDisplay. XInitThreads enables XCB sequence
    // tracking assertions that have known bugs in the Xlib-XCB bridge.

    // Create plugin registry
    g_ctx->pluginRegistry = std::make_unique<PluginRegistry>();

    // Register LV2 factory (pass path from nativeSetLv2Path for extracted Guitarix/assets)
    auto lv2Factory = std::make_unique<LV2PluginFactory>(g_ctx->lv2Path, g_ctx->nativeLibDir, g_ctx->filesDir, g_ctx->pluginLibDir);
    g_ctx->pluginRegistry->registerFactory(std::move(lv2Factory));

#if HAS_VST_HOST
    // Register VST factory (full flavor only — VST hosting via wine + FEX).
    // Plugin enumeration reads filesDir/vst_plugins/registry.json which the
    // Manage VST UI (Phase D) writes on user import/remove.
    const std::string wineRoot   = g_ctx->filesDir + "/wine";
    // VstHostSetup.ensureWineRoot stages vst_host.exe variants directly
    // into filesDir/ (not filesDir/assets/) — match that.
    const std::string assetsDir  = g_ctx->filesDir;
    auto vstFactory = vsthost::createVstFactory(g_ctx->filesDir, wineRoot, assetsDir, g_ctx->nativeLibDir);
    g_ctx->pluginRegistry->registerFactory(std::move(vstFactory));
    LOGI("Registered VST factory (full flavor)");
#endif

    // Initialize all factories
    if (!g_ctx->pluginRegistry->initializeAll()) {
        LOGE("Failed to initialize plugin factories");
        return JNI_FALSE;
    }

    // Create audio engine
    g_ctx->directUsbOutput = std::make_unique<DirectUsbOutput>();
    g_ctx->audioEngine = std::make_unique<AudioEngine>();
    g_ctx->audioEngine->setDirectUsbOutput(g_ctx->directUsbOutput.get());

    // Plugin UI manager follows the master until path-aware editor migration.
    g_ctx->pluginUIManager = std::make_unique<guitarrackcraft::PluginUIManager>();
    auto masterChain = g_ctx->audioEngine->getRackGraph().getChain(kMasterPathId);
    g_ctx->pluginUIManager->setChain(masterChain.get());

    // Start the X11Worker thread for single-threaded X11 operations
    // This prevents xcb_xlib_threads_sequence_lost crashes by ensuring
    // all X11 calls (plugin instantiate, idle, cleanup) happen on one thread
    getX11Worker().start();
    LOGI("X11Worker started for single-threaded X11 operations");

    LOGI("Native engine initialized");
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginX11Display(
    JNIEnv*, jobject, jlong pathId, jint position) {
    if (!g_ctx || !g_ctx->audioEngine) return -1;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    auto* plugin = chain ? chain->getPlugin(position) : nullptr;
    return plugin ? plugin->getX11DisplayNumber() : -1;
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginEditorSize(
    JNIEnv*, jobject, jlong pathId, jint position) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    auto* plugin = chain ? chain->getPlugin(position) : nullptr;
    if (!plugin) return 0;
    const int64_t w = plugin->getEditorWidth();
    const int64_t h = plugin->getEditorHeight();
    return (w << 32) | (h & 0xffffffffLL);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRefreshPluginRegistry(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->pluginRegistry) {
        LOGE("nativeRefreshPluginRegistry: registry not initialized");
        return JNI_FALSE;
    }
    // Re-runs each factory's initialize() (which for VstFactory re-reads
    // filesDir/vst_plugins/registry.json) and rebuilds pluginCache_ from
    // their enumeratePlugins(). Called from Kotlin after the Manage VST UI
    // imports or removes a plugin so the rack browser sees the new entry
    // without an engine restart.
    const bool ok = g_ctx->pluginRegistry->initializeAll();
    LOGI("nativeRefreshPluginRegistry: ok=%d", ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeOpenDirectUsbOutput(
        JNIEnv* env, jobject thiz, jint fileDescriptor) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->openDirectUsbDevice(static_cast<int>(fileDescriptor))
        ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetDirectUsbInputChannelCount(
        JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->directUsbOutput) return 0;
    return static_cast<jint>(g_ctx->directUsbOutput->captureChannelCount());
}


JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStartDirectUsbSession(
        JNIEnv* env, jobject thiz, jint sampleRate, jint bitsPerSample,
        jint bytesPerSample, jint channels, jint inputChannel, jint outputPair,
        jint bufferFrames, jint periodMultiplier, jint watermarkFrames) {
    if (!g_ctx || !g_ctx->audioEngine || !g_ctx->directUsbOutput) return JNI_FALSE;
    return g_ctx->audioEngine->startDirectUsbSession(
        static_cast<float>(sampleRate),
        static_cast<int32_t>(bitsPerSample),
        static_cast<int32_t>(bytesPerSample),
        static_cast<int32_t>(channels),
        static_cast<int32_t>(inputChannel),
        static_cast<int32_t>(outputPair),
        static_cast<int32_t>(bufferFrames),
        static_cast<int32_t>(periodMultiplier),
        static_cast<int32_t>(watermarkFrames)
    ) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeFlushPgoProfile(
        JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (__llvm_profile_write_file == nullptr)
        return JNI_FALSE;
    return __llvm_profile_write_file() == 0 ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jintArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetDirectUsbOutputFormats(
        JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->directUsbOutput) return nullptr;
    const auto formats = g_ctx->directUsbOutput->enumerateFormats();
    jintArray out = env->NewIntArray(static_cast<jsize>(formats.size() * 4));
    if (!out) return nullptr;
    std::vector<jint> packed;
    packed.reserve(formats.size() * 4);
    for (const auto& f : formats) {
        packed.push_back(static_cast<jint>(f.sampleRateHz));
        packed.push_back(static_cast<jint>(f.bitsPerSample));
        packed.push_back(static_cast<jint>(f.bytesPerSample));
        packed.push_back(static_cast<jint>(f.channels));
    }
    if (!packed.empty()) env->SetIntArrayRegion(out, 0, static_cast<jsize>(packed.size()), packed.data());
    return out;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStopDirectUsbOutput(
        JNIEnv* env, jobject thiz) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsDirectUsbOutputStreaming(
        JNIEnv* env, jobject thiz) {
    return g_ctx && g_ctx->directUsbOutput &&
        g_ctx->directUsbOutput->isStreaming() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlongArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetDirectUsbStats(
        JNIEnv* env, jobject thiz) {
    constexpr jsize kStatCount = 39;
    jlong values[kStatCount] = {};
    if (g_ctx && g_ctx->directUsbOutput) {
        const auto capture = g_ctx->directUsbOutput->captureStats();
        const auto transport = g_ctx->directUsbOutput->transportStats();
        values[0] = static_cast<jlong>(capture.sequence);
        values[1] = static_cast<jlong>(capture.overruns);
        values[2] = static_cast<jlong>(capture.underruns);
        values[3] = static_cast<jlong>(transport.fifoDepth);
        values[4] = static_cast<jlong>(transport.fallbackPackets);
        values[5] = static_cast<jlong>(transport.captureTransferErrors);
        values[6] = static_cast<jlong>(transport.playbackTransferErrors);
        values[7] = static_cast<jlong>(transport.ringFrames);
        values[8] = static_cast<jlong>(transport.captureRingFrames);
        values[9] = static_cast<jlong>(transport.lifecycleFailures);
        values[10] = transport.transportFailed ? 1 : 0;
        values[11] = transport.eventThreadUrgentAudio ? 1 : 0;
        values[12] = g_ctx->audioEngine &&
                g_ctx->audioEngine->isDirectUsbRenderUrgentAudio() ? 1 : 0;
        values[13] = static_cast<jlong>(g_ctx->directUsbOutput->writtenFrames());
        values[14] = static_cast<jlong>(g_ctx->directUsbOutput->playedFrames());
        values[15] = static_cast<jlong>(g_ctx->directUsbOutput->xrunCount());
        values[37] = static_cast<jlong>(
            g_ctx->directUsbOutput->playbackBackpressureCount());
        values[16] = g_ctx->audioEngine
            ? static_cast<jlong>(g_ctx->audioEngine->directUsbCaptureWaitTimeouts()) : 0;
        values[17] = g_ctx->audioEngine
            ? static_cast<jlong>(g_ctx->audioEngine->directUsbWriteWaitTimeouts()) : 0;
        if (g_ctx->audioEngine) {
            const auto stats = g_ctx->audioEngine->getDirectUsbRuntimeStats();
            values[18] = 5;
            values[19] = static_cast<jlong>(stats.sessionId);
            values[20] = static_cast<jlong>(stats.state);
            values[21] = static_cast<jlong>(stats.failureCode);
            values[22] = static_cast<jlong>(g_ctx->audioEngine->getSampleRate());
            values[23] = static_cast<jlong>(stats.effectiveQuantum);
            values[24] = static_cast<jlong>(stats.requestedPeriodMultiplier);
            values[25] = static_cast<jlong>(stats.startupPrimeFrames);
            values[26] = static_cast<jlong>(stats.steadyTargetFrames);
            values[27] = static_cast<jlong>(stats.queuedOutFrames);
            values[28] = static_cast<jlong>(stats.captureTransferFrames);
            values[29] = static_cast<jlong>(stats.lastDspNanoseconds);
            values[30] = static_cast<jlong>(stats.peakDspNanoseconds);
            const uint64_t hostFrames = std::max<uint32_t>(
                stats.effectiveQuantum,
                std::max(stats.captureRingFrames, stats.captureTransferFrames));
            values[31] = static_cast<jlong>(
                hostFrames + stats.playbackRingFrames + stats.queuedOutFrames);
            values[32] = static_cast<jlong>(
                stats.playbackXruns + stats.captureOverruns + stats.captureUnderruns);
            values[33] = static_cast<jlong>(stats.lastCycleNanoseconds);
            values[34] = static_cast<jlong>(stats.peakCycleNanoseconds);
            values[35] = static_cast<jlong>(stats.deadlineBudgetNanoseconds);
            values[36] = static_cast<jlong>(stats.deadlineMisses);
            values[38] = stats.performanceHintActive ? 1 : 0;
        }
    }
    jlongArray out = env->NewLongArray(kStatCount);
    if (out) env->SetLongArrayRegion(out, 0, kStatCount, values);
    return out;
}
JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetDirectUsbErrorDetail(
        JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->directUsbOutput) return env->NewStringUTF("");
    const std::string detail = g_ctx->directUsbOutput->lastErrorDetail();
    return env->NewStringUTF(detail.c_str());
}



JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStopEngine(JNIEnv* env, jobject thiz) {
    LOGI("nativeStopEngine CALLED tid=%ld (Java requested direct USB stop)", getTid());
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->stop();
    }
    LOGI("nativeStopEngine RETURNED tid=%ld", getTid());
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsEngineRunning(JNIEnv* env, jobject thiz) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetSampleRate(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getSampleRate();
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetBufferFrameCount(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0;
    }
    return static_cast<jint>(g_ctx->audioEngine->getCallbackFrameCount());
}


JNIEXPORT jdouble JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetLatencyMs(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0;
    }
    return g_ctx->audioEngine->getLatencyMs();
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetInputLevel(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getInputLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetOutputLevel(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getOutputLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetCpuLoad(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getCpuLoad();
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetXRunCount(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0;
    }
    return g_ctx->audioEngine->getXRunCount();
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsInputClipping(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->isInputClipping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsOutputClipping(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->isOutputClipping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeResetClipping(JNIEnv* env, jobject thiz) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->resetClipping();
    }
}

// Helper function to create PortInfo object (uses cached JNI refs)
jobject createPortInfoObject(JNIEnv* env, const PortInfo& port) {
    if (!ensureJniCache(env)) return nullptr;

    // Build scale points list
    jobject scalePointsList = env->NewObject(g_jni.arrayListClass, g_jni.arrayListCtor,
        static_cast<jint>(port.scalePoints.size()));
    if (scalePointsList && !port.scalePoints.empty() && g_jni.scalePointClass && g_jni.scalePointCtor) {
        for (const auto& sp : port.scalePoints) {
            jstring labelStr = env->NewStringUTF(sp.label.c_str());
            jobject scalePointObj = env->NewObject(g_jni.scalePointClass, g_jni.scalePointCtor,
                labelStr, static_cast<jfloat>(sp.value));
            env->DeleteLocalRef(labelStr);
            if (scalePointObj) {
                env->CallBooleanMethod(scalePointsList, g_jni.arrayListAdd, scalePointObj);
                env->DeleteLocalRef(scalePointObj);
            }
        }
    }

    jstring nameStr = env->NewStringUTF(port.name.c_str());
    jstring symbolStr = env->NewStringUTF(port.symbol.c_str());

    jobject portObj = env->NewObject(g_jni.portInfoClass, g_jni.portInfoCtor,
        static_cast<jint>(port.index),
        nameStr,
        symbolStr,
        static_cast<jboolean>(port.isInput ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isAudio ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isControl ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isToggle ? JNI_TRUE : JNI_FALSE),
        static_cast<jfloat>(port.defaultValue),
        static_cast<jfloat>(port.minValue),
        static_cast<jfloat>(port.maxValue),
        scalePointsList
    );

    env->DeleteLocalRef(nameStr);
    env->DeleteLocalRef(symbolStr);
    if (scalePointsList) env->DeleteLocalRef(scalePointsList);

    return portObj;
}

// Helper function to convert PluginInfo to Java object (uses cached JNI refs)
jobject createPluginInfoObject(JNIEnv* env, const PluginInfo& info) {
    if (!ensureJniCache(env)) return nullptr;

    jobject obj = env->NewObject(g_jni.pluginInfoClass, g_jni.pluginInfoCtor);
    if (!obj) return nullptr;

    auto setString = [&](jfieldID field, const std::string& val) {
        if (!field || val.empty()) return;
        jstring s = env->NewStringUTF(val.c_str());
        env->SetObjectField(obj, field, s);
        env->DeleteLocalRef(s);
    };

    setString(g_jni.piId, info.id);
    setString(g_jni.piName, info.name);
    setString(g_jni.piFormat, info.format);
    setString(g_jni.piModguiBasePath, info.modguiBasePath);
    setString(g_jni.piModguiIconTemplate, info.modguiIconTemplate);
    setString(g_jni.piX11UiBinaryPath, info.x11UiBinaryPath);
    setString(g_jni.piX11UiUri, info.x11UiUri);

    if (g_jni.piHasX11Ui)
        env->SetBooleanField(obj, g_jni.piHasX11Ui, info.hasX11Ui ? JNI_TRUE : JNI_FALSE);

    // Create ports list
    if (g_jni.piPorts && !info.ports.empty()) {
        jobject portsList = env->NewObject(g_jni.arrayListClass, g_jni.arrayListCtor,
            static_cast<jint>(info.ports.size()));
        for (const auto& port : info.ports) {
            jobject portObj = createPortInfoObject(env, port);
            if (portObj) {
                env->CallBooleanMethod(portsList, g_jni.arrayListAdd, portObj);
                env->DeleteLocalRef(portObj);
            }
        }
        env->SetObjectField(obj, g_jni.piPorts, portsList);
        env->DeleteLocalRef(portsList);
    }

    return obj;
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetAvailablePlugins(JNIEnv* env, jobject thiz) {
    if (!ensureJniCache(env)) return nullptr;

    std::vector<PluginInfo> plugins;
    if (g_ctx && g_ctx->pluginRegistry) {
        plugins = g_ctx->pluginRegistry->getAllPlugins();
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(plugins.size()), g_jni.pluginInfoClass, nullptr);
    if (!result) {
        return nullptr;
    }

    for (size_t i = 0; i < plugins.size(); ++i) {
        jobject pluginObj = createPluginInfoObject(env, plugins[i]);
        if (pluginObj) {
            env->SetObjectArrayElement(result, static_cast<jsize>(i), pluginObj);
            env->DeleteLocalRef(pluginObj);
        }
    }

    return result;
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeAddPluginToRack(
    JNIEnv* env, jobject, jlong pathId, jstring pluginId, jint position) {
    if (!g_ctx || !g_ctx->pluginRegistry || !g_ctx->audioEngine || !pluginId) return -1;
    const char* id = env->GetStringUTFChars(pluginId, nullptr);
    if (!id) return -1;
    std::string fullId(id);
    env->ReleaseStringUTFChars(pluginId, id);
    auto plugin = g_ctx->pluginRegistry->createPlugin(fullId);
    if (!plugin) return -1;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    return chain ? chain->addPlugin(std::move(plugin), position) : -1;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRemovePluginFromRack(
    JNIEnv*, jobject, jlong pathId, jint position) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    return chain && chain->removePlugin(position) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeReorderRack(
    JNIEnv*, jobject, jlong pathId, jint fromPos, jint toPos) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    return chain && chain->reorderPlugins(fromPos, toPos) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetPluginFilePath(
    JNIEnv* env, jobject, jlong pathId, jint pluginIndex, jstring propertyUri, jstring filePath) {
    if (!g_ctx || !g_ctx->audioEngine || !propertyUri || !filePath) return;
    const char* property = env->GetStringUTFChars(propertyUri, nullptr);
    const char* path = env->GetStringUTFChars(filePath, nullptr);
    if (property && path) {
        std::lock_guard lock(g_ctx->rackControlMutex);
        if (auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId))) {
            chain->setPluginFilePath(pluginIndex, property, path);
        }
    }
    if (property) env->ReleaseStringUTFChars(propertyUri, property);
    if (path) env->ReleaseStringUTFChars(filePath, path);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetParameter(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex, jint portIndex, jfloat value) {
    if (!g_ctx || !g_ctx->audioEngine) return;
    std::lock_guard lock(g_ctx->rackControlMutex);
    if (auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId))) {
        chain->setParameter(pluginIndex, static_cast<uint32_t>(portIndex), value);
    }
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetParameter(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex, jint portIndex) {
    if (!g_ctx || !g_ctx->audioEngine) return 0.0f;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    return chain ? chain->getParameter(pluginIndex, static_cast<uint32_t>(portIndex)) : 0.0f;
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeAddTrack(JNIEnv*, jobject) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return static_cast<jlong>(g_ctx->audioEngine->getRackGraph().addTrack());
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRemoveTrack(JNIEnv*, jobject, jlong trackId) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().removeTrack(trackId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackVolume(
    JNIEnv*, jobject, jlong trackId, jfloat volume) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackVolume(trackId, volume) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackInputArmed(
    JNIEnv*, jobject, jlong trackId, jboolean armed) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputArmed(trackId, armed == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeLoadTrackWav(
    JNIEnv* env, jobject, jlong trackId, jstring path, jstring displayName) {
    if (!g_ctx || !g_ctx->audioEngine || !path || !displayName) return JNI_FALSE;
    const char* filePath = env->GetStringUTFChars(path, nullptr);
    const char* name = env->GetStringUTFChars(displayName, nullptr);
    const bool ok = filePath && name && g_ctx->audioEngine->loadTrackWav(trackId, filePath, name);
    if (filePath) env->ReleaseStringUTFChars(path, filePath);
    if (name) env->ReleaseStringUTFChars(displayName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeUnloadTrackWav(JNIEnv*, jobject, jlong trackId) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->unloadTrackWav(trackId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeClearTrackWavs(JNIEnv*, jobject) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->getRackGraph().clearTrackWavs() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTransportPlaying(JNIEnv*, jobject, jboolean playing) {
    return g_ctx && g_ctx->audioEngine &&
        g_ctx->audioEngine->getRackGraph().setTransportPlaying(playing == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTransportBpm(JNIEnv*, jobject, jdouble bpm) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    g_ctx->audioEngine->getRackGraph().setBeatsPerMinute(bpm);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRestartTransport(JNIEnv*, jobject) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->isRunning() &&
        g_ctx->audioEngine->getRackGraph().restartTransport() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTransportLooping(JNIEnv*, jobject, jboolean looping) {
    if (g_ctx && g_ctx->audioEngine) g_ctx->audioEngine->getRackGraph().setTransportLooping(looping == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackSize(JNIEnv*, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    return chain ? static_cast<jint>(chain->getSize()) : 0;
}

JNIEXPORT jobject JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginInfo(JNIEnv* env, jobject, jlong pathId, jint index) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    IPlugin* plugin = chain ? chain->getPlugin(index) : nullptr;
    return plugin ? createPluginInfoObject(env, plugin->getInfo()) : nullptr;
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginInstanceId(JNIEnv*, jobject, jlong pathId, jint index) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    return chain ? static_cast<jlong>(chain->getPluginInstanceId(index)) : 0;
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPlugins(
    JNIEnv* env, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    if (!chain) return nullptr;
    jclass entryClass = env->FindClass("com/vibes/dsp/engine/RackPluginEntry");
    if (!entryClass) return nullptr;
    jmethodID ctor = env->GetMethodID(
        entryClass, "<init>", "(IJLcom/vibes/dsp/engine/PluginInfo;)V");
    if (!ctor) return nullptr;
    const size_t size = chain->getSize();
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(size), entryClass, nullptr);
    for (size_t index = 0; index < size; ++index) {
        IPlugin* plugin = chain->getPlugin(static_cast<int>(index));
        if (!plugin) continue;
        jobject info = createPluginInfoObject(env, plugin->getInfo());
        jobject entry = env->NewObject(entryClass, ctor, static_cast<jint>(index),
                                       static_cast<jlong>(chain->getPluginInstanceId(index)), info);
        env->SetObjectArrayElement(result, static_cast<jsize>(index), entry);
        env->DeleteLocalRef(info);
        env->DeleteLocalRef(entry);
    }
    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTracks(JNIEnv* env, jobject) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    const auto tracks = g_ctx->audioEngine->getRackGraph().getTracks();
    jclass clazz = env->FindClass("com/vibes/dsp/engine/RackTrackInfo");
    if (!clazz) return nullptr;
    jmethodID ctor = env->GetMethodID(clazz, "<init>", "(JFZZLjava/lang/String;D)V");
    if (!ctor) return nullptr;
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(tracks.size()), clazz, nullptr);
    for (size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        jstring name = env->NewStringUTF(track.wavDisplayName.c_str());
        jobject item = env->NewObject(clazz, ctor, static_cast<jlong>(track.id), track.volume,
                                      track.inputArmed ? JNI_TRUE : JNI_FALSE,
                                      track.wavLoaded ? JNI_TRUE : JNI_FALSE, name,
                                      track.wavDurationSec);
        env->SetObjectArrayElement(result, static_cast<jsize>(index), item);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(item);
    }
    return result;
}

JNIEXPORT jobject JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTransportInfo(JNIEnv* env, jobject) {
    const TransportSnapshot state = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getTransportSnapshot() : TransportSnapshot{};
    jclass clazz = env->FindClass("com/vibes/dsp/engine/TransportInfo");
    if (!clazz) return nullptr;
    jmethodID ctor = env->GetMethodID(clazz, "<init>", "(ZZDDIDJJ)V");
    return ctor ? env->NewObject(clazz, ctor, state.playing ? JNI_TRUE : JNI_FALSE,
                                 state.looping ? JNI_TRUE : JNI_FALSE, state.positionSec,
                                 state.durationSec, static_cast<jint>(state.loadedTrackCount),
                                 state.beatsPerMinute, static_cast<jlong>(state.samplePosition),
                                 static_cast<jlong>(state.transportFrame)) : nullptr;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeBeginCreatePluginUI(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex, jlong pluginInstanceId,
    jlong uiInstanceId, jint displayNumber) {
    (void)uiInstanceId;
    auto chain = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getChain(pathId) : nullptr;
    if (!chain || pluginIndex < 0 ||
        static_cast<size_t>(pluginIndex) >= chain->getSize() ||
        static_cast<jlong>(chain->getPluginInstanceId(pluginIndex)) != pluginInstanceId) return;
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates()[displayNumber].phase = DisplayState::Phase::Creating;
        displayStates()[displayNumber].pluginIndex = pluginIndex;
        displayStates()[displayNumber].detachPending = false;
    }
    setCreatingPluginUI(true);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeCreatePluginUI(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex, jlong pluginInstanceId,
    jlong uiInstanceId, jint displayNumber, jlong parentWindowId) {
    (void)uiInstanceId;
    LOGI("nativeCreatePluginUI ENTER tid=%ld path=%ld plugin=%d display=%d parent=0x%lx",
         getTid(), static_cast<long>(pathId), pluginIndex, displayNumber,
         (unsigned long)parentWindowId);
    auto chain = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getChain(pathId) : nullptr;
    if (!g_ctx || !g_ctx->audioEngine || !g_ctx->pluginUIManager ||
        !chain || pluginIndex < 0 ||
        static_cast<size_t>(pluginIndex) >= chain->getSize() ||
        static_cast<jlong>(chain->getPluginInstanceId(pluginIndex)) != pluginInstanceId) {
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
        setCreatingPluginUI(false);
        return JNI_FALSE;
    }
    
    bool result = g_ctx->pluginUIManager->createPluginUI(
        pluginIndex,
        displayNumber,
        (unsigned long)parentWindowId,
        ensureCtx()->nativeLibDir,
        ensureCtx()->x11LibsDir
    );
    
    // Check if detach was requested during creation
    bool detachPending = false;
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            detachPending = it->second.detachPending;
            if (result) {
                it->second.phase = DisplayState::Phase::Ready;
            } else {
                it->second.phase = DisplayState::Phase::None;
            }
        }
    }
    
    if (detachPending && result) {
        /* Surface was destroyed while we were creating the UI.
         * Destroy the plugin UI now that creation is complete. */
        LOGI("nativeCreatePluginUI: deferred detach detected, destroying plugin UI");
        g_ctx->pluginUIManager->destroyPluginUI(pluginIndex);
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
    }
    
    /* Clear the global creating flag when done */
    setCreatingPluginUI(false);
    
    LOGI("nativeCreatePluginUI EXIT tid=%ld result=%s", getTid(), result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDestroyPluginUI(
    JNIEnv*, jobject, jlong pathId, jlong pluginInstanceId, jlong uiInstanceId) {
    (void)uiInstanceId;
    auto chain = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getChain(pathId) : nullptr;
    if (!chain) return;
    const int pluginIndex = [&]() {
        for (size_t i = 0; i < chain->getSize(); ++i)
            if (static_cast<jlong>(chain->getPluginInstanceId(static_cast<int>(i))) == pluginInstanceId)
                return static_cast<int>(i);
        return -1;
    }();
    if (pluginIndex < 0) return;
    g_ctx->pluginUIManager->destroyPluginUI(pluginIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIdlePluginUIs(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        return g_ctx->pluginUIManager->idleAllUIs() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

// --- X11 native display (EGL + ANativeWindow) ---

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeAttachSurfaceToDisplay(JNIEnv* env, jobject thiz, jint displayNumber, jobject surface, jint width, jint height) {
    if (!surface || width <= 0 || height <= 0) {
        LOGE("nativeAttachSurfaceToDisplay: invalid surface or size");
        return 0;
    }
    LOGI("nativeAttachSurfaceToDisplay display=%d width=%d height=%d", displayNumber, width, height);
    
    /* Initialize display state BEFORE attaching surface */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates()[displayNumber].phase = DisplayState::Phase::Attached;
        displayStates()[displayNumber].pluginIndex = -1;
        displayStates()[displayNumber].detachPending = false;
    }
    
    X11NativeDisplay* disp = getOrCreateX11Display(displayNumber);
    if (!disp->attachSurface(env, surface, width, height)) {
        LOGE("nativeAttachSurfaceToDisplay: attach failed for display %d", displayNumber);
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates().erase(displayNumber);
        return 0;
    }
    
    jlong rootId = static_cast<jlong>(disp->getRootWindowId());
    LOGI("nativeAttachSurfaceToDisplay display=%d -> rootId=%ld", displayNumber, (long)rootId);
    return rootId;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSignalDetachSurfaceFromDisplay(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeSignalDetachSurfaceFromDisplay ENTER display=%d tid=%ld", displayNumber, getTid());
    
    /* Always defer when creating - never close X connection during plugin creation.
     * This prevents "X connection closed" crashes when the plugin is still
     * initializing its X11 connection. */
    auto phase = getDisplayPhase(displayNumber);
    if (phase == DisplayState::Phase::Creating) {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            it->second.detachPending = true;
        }
        LOGI("nativeSignalDetachSurfaceFromDisplay: DEFERRED (creating) display=%d", displayNumber);
        return JNI_TRUE;
    }
    
    /* For other phases, mark for destruction but don't actually close yet.
     * The Kotlin layer will call nativeDetachAndDestroyX11DisplayIfExists
     * after the appropriate delay. */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            it->second.phase = DisplayState::Phase::Destroying;
        }
    }
    
    LOGI("nativeSignalDetachSurfaceFromDisplay: marked destroying display=%d", displayNumber);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStopX11RenderThreadOnly(JNIEnv* env, jobject thiz, jint displayNumber) {
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) disp->stopRenderThreadOnly();
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDetachSurfaceFromDisplay(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeDetachSurfaceFromDisplay ENTER display=%d tid=%ld", displayNumber, getTid());
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) disp->detachSurface();
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDestroyX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    destroyX11Display(displayNumber);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDetachAndDestroyX11DisplayIfExists(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeDetachAndDestroyX11DisplayIfExists ENTER display=%d tid=%ld", displayNumber, getTid());
    
    auto phase = getDisplayPhase(displayNumber);
    
    /* If still creating, defer teardown to avoid closing X connection during plugin init. */
    if (phase == DisplayState::Phase::Creating) {
        LOGI("nativeDetachAndDestroyX11DisplayIfExists: SKIP (creating) display=%d", displayNumber);
        return;
    }
    
    /* Mark as destroying and proceed with teardown */
    setDisplayPhase(displayNumber, DisplayState::Phase::Destroying);
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeDetachAndDestroyX11DisplayIfExists: detaching and destroying display=%d", displayNumber);
        disp->detachSurface();
        destroyX11Display(displayNumber);
    }
    
    /* Clean up state */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates().erase(displayNumber);
    }
    
    LOGI("nativeDetachAndDestroyX11DisplayIfExists EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeHideX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeHideX11Display ENTER display=%d tid=%ld", displayNumber, getTid());
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeHideX11Display: stopping render thread for display=%d", displayNumber);
        // Stop the render thread to prevent eglSwapBuffers and driver mutex issues
        disp->stopRenderThreadOnly();
        LOGI("nativeHideX11Display: display=%d hidden (render thread stopped)", displayNumber);
    } else {
        LOGI("nativeHideX11Display: display=%d not found", displayNumber);
    }
    
    LOGI("nativeHideX11Display EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeResumeX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeResumeX11Display ENTER display=%d tid=%ld", displayNumber, getTid());
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeResumeX11Display: starting render thread for display=%d", displayNumber);
        // Restart the render thread to resume rendering
        disp->startRenderThread();
        LOGI("nativeResumeX11Display: display=%d resumed (render thread started)", displayNumber);
    } else {
        LOGI("nativeResumeX11Display: display=%d not found", displayNumber);
    }
    
    LOGI("nativeResumeX11Display EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetSurfaceSize(JNIEnv* env, jobject thiz, jint displayNumber, jint width, jint height) {
    withDisplaySetSurfaceSize(displayNumber, width, height);
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeInjectTouch(JNIEnv* env, jobject thiz, jint displayNumber, jint action, jint x, jint y) {
    withDisplayInjectTouch(displayNumber, action, x, y);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsWidgetAtPoint(JNIEnv* env, jobject thiz, jint displayNumber, jint x, jint y) {
    return withDisplayIsWidgetAtPoint(displayNumber, x, y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRequestX11Frame(JNIEnv* env, jobject thiz, jint displayNumber) {
    withDisplayRequestFrame(displayNumber);
}

JNIEXPORT jintArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetX11PluginSize(JNIEnv* env, jobject thiz, jint displayNumber) {
    int w = 0, h = 0;
    withDisplayGetPluginSize(displayNumber, w, h);
    jintArray result = env->NewIntArray(2);
    if (result) {
        jint arr[2] = { w, h };
        env->SetIntArrayRegion(result, 0, 2, arr);
    }
    return result;
}

JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetX11UIScale(JNIEnv* env, jobject thiz, jint displayNumber) {
    return withDisplayGetUIScale(displayNumber);
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativePollFileRequest(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->pluginUIManager) return nullptr;

    guitarrackcraft::PluginUIManager::FileRequest req;
    if (!g_ctx->pluginUIManager->pollFileRequest(req)) return nullptr;

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(2, stringClass, nullptr);
    if (!result) return nullptr;

    jstring indexStr = env->NewStringUTF(std::to_string(req.pluginIndex).c_str());
    jstring uriStr = env->NewStringUTF(req.propertyUri.c_str());
    env->SetObjectArrayElement(result, 0, indexStr);
    env->SetObjectArrayElement(result, 1, uriStr);
    env->DeleteLocalRef(indexStr);
    env->DeleteLocalRef(uriStr);
    env->DeleteLocalRef(stringClass);

    return result;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDeliverFileToPluginUI(
    JNIEnv* env, jobject, jlong pathId, jint pluginIndex, jstring propertyUri, jstring filePath)
{
    if (!g_ctx || !g_ctx->pluginUIManager) return;
    auto chain = g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getChain(pathId) : nullptr;
    if (!chain || pluginIndex < 0 ||
        static_cast<size_t>(pluginIndex) >= chain->getSize()) return;

    const char* propStr = env->GetStringUTFChars(propertyUri, nullptr);
    const char* pathStr = env->GetStringUTFChars(filePath, nullptr);
    if (propStr && pathStr) {
        g_ctx->pluginUIManager->deliverFileToUI(
            pluginIndex, std::string(propStr), std::string(pathStr));
    }
    if (propStr) env->ReleaseStringUTFChars(propertyUri, propStr);
    if (pathStr) env->ReleaseStringUTFChars(filePath, pathStr);
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativePollVstFilePickerRequest(
    JNIEnv* env, jobject, jlong pathId, jint pluginIndex)
{
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    IPlugin* plugin = chain ? chain->getPlugin(pluginIndex) : nullptr;
    if (!plugin) return nullptr;

    NativeFilePickerRequest req;
    if (!plugin->pollNativeFilePicker(req)) return nullptr;

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(6, stringClass, nullptr);
    if (!result) return nullptr;

    auto setElement = [&](jsize index, const std::string& value) {
        jstring str = env->NewStringUTF(value.c_str());
        env->SetObjectArrayElement(result, index, str);
        env->DeleteLocalRef(str);
    };

    setElement(0, std::to_string(req.sequence));
    setElement(1, req.title);
    setElement(2, req.filterPatterns);
    setElement(3, req.initialDir);
    setElement(4, req.copyDirLinux);
    setElement(5, req.copyDirWindows);
    env->DeleteLocalRef(stringClass);

    return result;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRespondVstFilePicker(
    JNIEnv* env, jobject, jlong pathId, jint pluginIndex, jint sequence, jboolean cancelled, jstring windowsPath)
{
    if (!g_ctx || !g_ctx->audioEngine) return;
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    IPlugin* plugin = chain ? chain->getPlugin(pluginIndex) : nullptr;
    if (!plugin) return;

    std::string path;
    if (windowsPath) {
        const char* pathStr = env->GetStringUTFChars(windowsPath, nullptr);
        if (pathStr) {
            path = pathStr;
            env->ReleaseStringUTFChars(windowsPath, pathStr);
        }
    }

    plugin->respondNativeFilePicker(
        static_cast<uint32_t>(sequence),
        cancelled == JNI_TRUE,
        path);
}




} // extern "C"
