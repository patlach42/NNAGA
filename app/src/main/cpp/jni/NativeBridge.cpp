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

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <signal.h>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "../engine/AudioEngine.h"
#include "../plugin/PluginUIGuard.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/IPlugin.h"
#include "../plugin/IPluginFactory.h"
#include "../plugin/lv2/LV2PluginFactory.h"
#include "../plugin/native/NativePluginFactory.h"

#include "../jsfx/JsfxPluginFactory.h"
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
#include "NativeContextAccess.h"
#include <liblowlatencyaudio/ThreadUtils.h>

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
    std::unordered_map<jlong, std::unique_ptr<PluginUIManager>> pluginUIManagers;
    // Keep replaced chains alive while their UI callbacks are synchronously
    // torn down after a full-rack restore.
    std::unordered_map<jlong, std::shared_ptr<PluginChain>> pendingRackRestoreChains;
    std::mutex rackControlMutex;
    std::string lv2Path;
    std::string nativeLibDir;
    std::string pluginLibDir;
    std::string filesDir;
    std::string x11LibsDir;
    std::string jsfxRoot;
};

static NativeContext* g_ctx = nullptr;

// NativeContext is process-local and must be published exactly once.  The
// context owns the live audio graph, so replacing it during Activity
// recreation would tear down the running engine.
static std::once_flag g_ctxOnce;

static NativeContext* ensureCtx() {
    std::call_once(g_ctxOnce, [] { g_ctx = new NativeContext(); });
    return g_ctx;
}

NativeContext* nnagaNativeContext() noexcept { return g_ctx; }
PluginRegistry* nnagaNativePluginRegistry() noexcept { return g_ctx ? g_ctx->pluginRegistry.get() : nullptr; }
RackGraph* nnagaNativeRackGraph() noexcept {
    return g_ctx && g_ctx->audioEngine ? &g_ctx->audioEngine->getRackGraph() : nullptr;
}
std::mutex* nnagaNativeRackMutex() noexcept { return g_ctx ? &g_ctx->rackControlMutex : nullptr; }
bool nnagaNativeEngineRunning() noexcept { return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->isRunning(); }
const std::string& nnagaNativeJsfxRoot() noexcept { static const std::string empty; return g_ctx ? g_ctx->jsfxRoot : empty; }

void nnagaNativeRebindPluginUIManager(int64_t pathId, PluginChain* chain) noexcept {
    if (!g_ctx) return;
    auto it = g_ctx->pluginUIManagers.find(static_cast<jlong>(pathId));
    if (it != g_ctx->pluginUIManagers.end()) {
        it->second->rebindChain(chain);
    }
}

void nnagaNativePrepareRackStateImport() noexcept {
    if (!g_ctx || !g_ctx->audioEngine) return;
    g_ctx->pendingRackRestoreChains.clear();
    auto& graph = g_ctx->audioEngine->getRackGraph();
    for (const auto& manager : g_ctx->pluginUIManagers) {
        g_ctx->pendingRackRestoreChains.emplace(
            manager.first, graph.getChain(static_cast<RackPathId>(manager.first)));
    }
}

void nnagaNativeCommitRackStateImport() noexcept {
    if (!g_ctx || !g_ctx->audioEngine) return;
    auto& graph = g_ctx->audioEngine->getRackGraph();
    // Do not release the old chains until every manager has completed its
    // synchronous callback/UI teardown.
    for (const auto& manager : g_ctx->pluginUIManagers) {
        const auto replacement = graph.getChain(static_cast<RackPathId>(manager.first));
        manager.second->rebindChain(replacement.get());
    }
    g_ctx->pendingRackRestoreChains.clear();
}

void nnagaNativeAbortRackStateImport() noexcept {
    if (g_ctx) g_ctx->pendingRackRestoreChains.clear();
}

static PluginUIManager* getPluginUIManagerLocked(NativeContext& ctx, jlong pathId,
                                                  PluginChain* chain) {
    auto it = ctx.pluginUIManagers.find(pathId);
    if (it != ctx.pluginUIManagers.end()) return it->second.get();
    auto manager = std::make_unique<PluginUIManager>();
    manager->setPathId(static_cast<int64_t>(pathId));
    manager->setChain(chain);
    auto* result = manager.get();
    ctx.pluginUIManagers.emplace(pathId, std::move(manager));
    return result;
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
    jfieldID piOriginPath = nullptr;
    jfieldID piPorts = nullptr;
    jfieldID piModguiBasePath = nullptr;
    jfieldID piModguiIconTemplate = nullptr;
    jfieldID piHasX11Ui = nullptr;
    jfieldID piX11UiBinaryPath = nullptr;
    jfieldID piParameterMetadataRevision = nullptr;
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
        "(ILjava/lang/String;Ljava/lang/String;ZZZZFFFLjava/util/List;Ljava/lang/String;IZ)V");

    g_jni.pluginInfoClass = cache("com/vibes/dsp/engine/PluginInfo");
    if (!g_jni.pluginInfoClass) return false;
    g_jni.pluginInfoCtor = env->GetMethodID(g_jni.pluginInfoClass, "<init>", "()V");
    g_jni.piId = env->GetFieldID(g_jni.pluginInfoClass, "id", "Ljava/lang/String;");
    g_jni.piName = env->GetFieldID(g_jni.pluginInfoClass, "name", "Ljava/lang/String;");
    g_jni.piFormat = env->GetFieldID(g_jni.pluginInfoClass, "format", "Ljava/lang/String;");
    g_jni.piOriginPath = env->GetFieldID(g_jni.pluginInfoClass, "originPath", "Ljava/lang/String;");
    g_jni.piPorts = env->GetFieldID(g_jni.pluginInfoClass, "ports", "Ljava/util/List;");
    g_jni.piModguiBasePath = env->GetFieldID(g_jni.pluginInfoClass, "modguiBasePath", "Ljava/lang/String;");
    g_jni.piModguiIconTemplate = env->GetFieldID(g_jni.pluginInfoClass, "modguiIconTemplate", "Ljava/lang/String;");
    g_jni.piHasX11Ui = env->GetFieldID(g_jni.pluginInfoClass, "hasX11Ui", "Z");
    g_jni.piX11UiBinaryPath = env->GetFieldID(g_jni.pluginInfoClass, "x11UiBinaryPath", "Ljava/lang/String;");
    g_jni.piParameterMetadataRevision = env->GetFieldID(
        g_jni.pluginInfoClass, "parameterMetadataRevision", "J");
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
static bool parseMidiFile(const std::string& path, const std::string& name, std::shared_ptr<MidiClip>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0) return false;
    auto u16=[&](size_t p)->uint16_t{return static_cast<uint16_t>((bytes[p]<<8)|bytes[p+1]);};
    auto u32=[&](size_t p)->uint32_t{return (static_cast<uint32_t>(bytes[p])<<24)|(static_cast<uint32_t>(bytes[p+1])<<16)|(static_cast<uint32_t>(bytes[p+2])<<8)|bytes[p+3];};
    const uint16_t format=u16(8), tracks=u16(10), division=u16(12);
    if ((format!=0&&format!=1)||tracks==0||(division&0x8000)||division==0) return false;
    struct Raw { uint64_t tick; MidiEvent ev; }; struct Tempo { uint64_t tick; uint32_t us; };
    std::vector<Raw> raw; std::vector<Tempo> tempos{{0,500000}}; size_t pos=14;
    for (uint16_t tr=0;tr<tracks;++tr) {
        if (pos+8>bytes.size()||std::memcmp(bytes.data()+pos,"MTrk",4)!=0) return false;
        const uint32_t len=u32(pos+4); pos+=8; if (len>bytes.size()-pos) return false; const size_t end=pos+len;
        uint64_t tick=0; uint8_t running=0;
        while (pos<end) {
            uint32_t delta=0; int n=0; uint8_t b;
            do { if(pos>=end||n++>=4)return false; b=bytes[pos++]; delta=(delta<<7)|(b&0x7f); } while(b&0x80);
            tick+=delta; if(pos>=end)return false; uint8_t status=bytes[pos++]; if(status<0x80){if(!running)return false;--pos;status=running;} else if(status<0xf0) running=status;
            if(status==0xff){if(pos>=end)return false;uint8_t meta=bytes[pos++];uint32_t ml=0;n=0;do{if(pos>=end||n++>=4)return false;b=bytes[pos++];ml=(ml<<7)|(b&0x7f);}while(b&0x80);if(ml>end-pos)return false;if(meta==0x51&&ml==3)tempos.push_back({tick,(uint32_t(bytes[pos])<<16)|(uint32_t(bytes[pos+1])<<8)|bytes[pos+2]});pos+=ml;continue;}
            if(status==0xf0||status==0xf7){uint32_t sl=0;n=0;do{if(pos>=end||n++>=4)return false;b=bytes[pos++];sl=(sl<<7)|(b&0x7f);}while(b&0x80);if(sl>end-pos)return false;pos+=sl;continue;}
            const uint8_t type=status&0xf0; if(type!=0x80&&type!=0x90&&type!=0xa0&&type!=0xb0&&type!=0xc0&&type!=0xd0&&type!=0xe0)return false;
            if(pos>=end)return false; uint8_t d1=bytes[pos++]; uint8_t d2=0; if(type!=0xc0&&type!=0xd0){if(pos>=end)return false;d2=bytes[pos++];}
            raw.push_back({tick,{0,status,d1,d2}});
        }
        pos=end;
    }
    std::stable_sort(tempos.begin(),tempos.end(),[](auto&a,auto&b){return a.tick<b.tick;}); std::sort(raw.begin(),raw.end(),[](auto&a,auto&b){return a.tick<b.tick;});
    auto clip=std::make_shared<MidiClip>(); clip->displayName=name; uint64_t lastTick=0, micros=0; uint32_t tempo=500000; size_t ti=0;
    while (ti+1<tempos.size() && tempos[ti+1].tick==0) ++ti;
    tempo=tempos[ti].us;
    if (tempo == 0) return false;
    clip->sourceBpm=60'000'000.0/static_cast<double>(tempo);
    ti=0;
    for(const auto& r:raw){while(ti+1<tempos.size()&&tempos[ti+1].tick<=r.tick){micros+=(tempos[ti+1].tick-lastTick)*tempo/division;lastTick=tempos[++ti].tick;tempo=tempos[ti].us;if(tempo==0)return false;} micros+=(r.tick-lastTick)*tempo/division;lastTick=r.tick; MidiTimedEvent e; e.microseconds=micros; e.event=r.ev; clip->events.push_back(e); clip->durationMicroseconds=std::max(clip->durationMicroseconds,e.microseconds+1);}
    out=std::move(clip); return true;
}


// Minimal SIGABRT handler: log to stderr (async-signal-safe) then re-raise so tombstone is still generated.
static void sigabrt_handler(int signum) {
    (void)signum;
    const char msg[] = "NNAGA: SIGABRT received (check tombstone for backtrace)\n";
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
Java_com_vibes_dsp_engine_NativeEngine_nativeSetJsfxRoot(JNIEnv* env, jobject, jstring path) {
    auto* ctx = ensureCtx();
    if (!path) { ctx->jsfxRoot.clear(); return; }
    const char* value = env->GetStringUTFChars(path, nullptr);
    if (value) { ctx->jsfxRoot = value; env->ReleaseStringUTFChars(path, value); }
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
Java_com_vibes_dsp_engine_NativeEngine_nativeValidateNativePlugin(JNIEnv* env, jobject, jstring path) {
    if (!path) return JNI_FALSE;
    const char* rawPath = env->GetStringUTFChars(path, nullptr);
    if (!rawPath) return JNI_FALSE;
    const std::string nativePath(rawPath);
    env->ReleaseStringUTFChars(path, rawPath);
    return validateNativePluginPath(nativePath) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeInit(JNIEnv* env, jobject thiz) {

    std::lock_guard<std::mutex> initLock(g_nativeInitMutex);
    NativeContext* ctx = ensureCtx();
    if (ctx->audioEngine && ctx->pluginRegistry && !ctx->pluginUIManagers.empty()) {
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

    g_ctx->pluginRegistry->registerFactory(
        std::make_unique<NativePluginFactory>(g_ctx->filesDir, g_ctx->nativeLibDir, g_ctx->pluginLibDir));

    if (!g_ctx->jsfxRoot.empty()) {
        const std::string dataRoot = std::filesystem::path(g_ctx->jsfxRoot).parent_path() / "Data";
        g_ctx->pluginRegistry->registerFactory(
            std::make_unique<JsfxPluginFactory>(g_ctx->jsfxRoot, dataRoot));
    }
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

    // Managers are created lazily per rack path so plugin indices cannot collide.
    // Initialize the master manager here to preserve existing behavior.
    {
        std::lock_guard lock(g_ctx->rackControlMutex);
        auto masterChain = g_ctx->audioEngine->getRackGraph().getChain(kMasterPathId);
        if (masterChain) getPluginUIManagerLocked(*g_ctx, kMasterPathId, masterChain.get());
    }

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
    if (!chain || position < 0 || static_cast<size_t>(position) >= chain->getSize()) return -1;
    return chain->visitPlugin(static_cast<size_t>(position),
                              [](IPlugin& plugin) { return plugin.getX11DisplayNumber(); });
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginEditorSize(
    JNIEnv*, jobject, jlong pathId, jint position) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    if (!chain || position < 0 || static_cast<size_t>(position) >= chain->getSize()) return 0;
    return chain->visitPlugin(static_cast<size_t>(position), [](IPlugin& plugin) -> jlong {
        const int64_t w = plugin.getEditorWidth();
        const int64_t h = plugin.getEditorHeight();
        return (w << 32) | (h & 0xffffffffLL);
    });
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
        JNIEnv* env, jobject thiz, jint fileDescriptor, jint driverCode) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->openDirectUsbDevice(
        static_cast<int>(fileDescriptor), static_cast<int>(driverCode)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetDirectUsbInputChannelCount(
        JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->directUsbOutput) return 0;
    return static_cast<jint>(g_ctx->directUsbOutput->captureChannelCount());
}


JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStartAndroidOboeSession(
        JNIEnv*, jobject, jint inputDeviceId, jint outputDeviceId, jint bufferFrames) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->startAndroidOboeSession(
        static_cast<int32_t>(inputDeviceId), static_cast<int32_t>(outputDeviceId),
        static_cast<int32_t>(bufferFrames)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStartDirectUsbSession(
        JNIEnv* env, jobject thiz, jint sampleRate, jint bitsPerSample,
        jint bytesPerSample, jint channels, jint outputPair,
        jint bufferFrames, jint periodMultiplier, jint playbackTargetFrames,
        jint startupPrimeFrames, jint writeHeadroomFrames, jint captureLimitFrames,
        jint transferCount, jint packetsPerTransfer, jint ringCapacityBytes,
        jboolean thermalSafetyEnabled) {
    if (!g_ctx || !g_ctx->audioEngine || !g_ctx->directUsbOutput) return JNI_FALSE;
    return g_ctx->audioEngine->startDirectUsbSession(
        static_cast<float>(sampleRate),
        static_cast<int32_t>(bitsPerSample),
        static_cast<int32_t>(bytesPerSample),
        static_cast<int32_t>(channels),
        static_cast<int32_t>(outputPair),
        static_cast<int32_t>(bufferFrames),
        static_cast<int32_t>(periodMultiplier),
        monotrypt::usb::UserspaceBufferConfig{
            static_cast<int>(playbackTargetFrames),
            static_cast<int>(startupPrimeFrames),
            static_cast<int>(writeHeadroomFrames),
            static_cast<int>(captureLimitFrames),
            static_cast<int>(transferCount),
            static_cast<int>(packetsPerTransfer),
            static_cast<size_t>(std::max(0, ringCapacityBytes))
        },
        thermalSafetyEnabled == JNI_TRUE
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

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeCloseDirectUsbOutput(
        JNIEnv* env, jobject thiz) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->closeDirectUsbDevice();
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
    constexpr jsize kStatCount = 48;
    jlong values[kStatCount] = {};
    if (g_ctx && g_ctx->directUsbOutput) {
        const auto capture = g_ctx->directUsbOutput->captureStats();
        const auto transport = g_ctx->directUsbOutput->transportStats();
        values[0] = static_cast<jlong>(capture.sequence);
        values[1] = static_cast<jlong>(capture.overruns);
        values[2] = static_cast<jlong>(capture.underruns);
        values[3] = static_cast<jlong>(transport.fifoDepth);
        values[4] = static_cast<jlong>(transport.deferredTransfers);
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
        values[39] = static_cast<jlong>(
            g_ctx->directUsbOutput->playbackSilentPacketCount());
        values[40] = static_cast<jlong>(
            g_ctx->directUsbOutput->playbackSilentFrameCount());
        values[41] = static_cast<jlong>(transport.metadataFifoOverruns);
        values[42] = static_cast<jlong>(transport.pendingDepth);
        values[43] = static_cast<jlong>(transport.pendingHighWater);
        values[44] = static_cast<jlong>(transport.zeroRunwayEvents);
        values[45] = static_cast<jlong>(transport.maxPendingAgeNs);
        values[16] = g_ctx->audioEngine
            ? static_cast<jlong>(g_ctx->audioEngine->directUsbCaptureWaitTimeouts()) : 0;
        values[17] = g_ctx->audioEngine
            ? static_cast<jlong>(g_ctx->audioEngine->directUsbWriteWaitTimeouts()) : 0;
        if (g_ctx->audioEngine) {
            const auto stats = g_ctx->audioEngine->getDirectUsbRuntimeStats();
            values[18] = 7;
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
                g_ctx->directUsbOutput->xrunCount() +
                capture.overruns + capture.underruns);
            values[33] = static_cast<jlong>(stats.lastCycleNanoseconds);
            values[34] = static_cast<jlong>(stats.peakCycleNanoseconds);
            values[35] = static_cast<jlong>(stats.deadlineBudgetNanoseconds);
            values[36] = static_cast<jlong>(stats.deadlineMisses);
            values[38] = stats.performanceHintActive ? 1 : 0;
            values[46] = stats.thermalSafetyEnabled ? 1 : 0;
            values[47] = stats.thermalSafetyActive ? 1 : 0;
        }
    }
    jlongArray out = env->NewLongArray(kStatCount);
    if (out) env->SetLongArrayRegion(out, 0, kStatCount, values);
    return out;
}
JNIEXPORT jlongArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRealtimeStats(
        JNIEnv* env, jobject) {
    // Schema v1; order mirrors AudioRealtimeStats.fromRaw.
    constexpr jsize kCount = 26;
    const auto stats = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRealtimeStatsSnapshot()
        : AudioEngine::RealtimeStatsSnapshot{};
    const jlong values[kCount] = {
        1, static_cast<jlong>(stats.callbackCount),
        static_cast<jlong>(stats.callbackFrames),
        static_cast<jlong>(stats.frameCapacityViolations),
        static_cast<jlong>(stats.inputUnderflowFrames),
        static_cast<jlong>(stats.inputOverflowFrames),
        static_cast<jlong>(stats.midiEventDrops),
        static_cast<jlong>(stats.planPublishDeferrals),
        static_cast<jlong>(stats.vstInputStarvations),
        static_cast<jlong>(stats.vstOutputUnderrunFrames),
        static_cast<jlong>(stats.vstGuestDeadlineMisses),
        static_cast<jlong>(stats.xRunCount),
        static_cast<jlong>(stats.audioApi),
        static_cast<jlong>(stats.sampleRateHz),
        static_cast<jlong>(stats.framesPerBurst),
        static_cast<jlong>(stats.bufferSize),
        static_cast<jlong>(stats.performanceMode),
        static_cast<jlong>(stats.sharingMode),
        static_cast<jlong>(stats.callbackFramesPerBurst),
        static_cast<jlong>(stats.activatedCapacity),
        static_cast<jlong>(stats.deviceId),
        static_cast<jlong>(stats.inputChannels),
        static_cast<jlong>(stats.lastCallbackNanoseconds),
        static_cast<jlong>(stats.peakCallbackNanoseconds),
        static_cast<jlong>(stats.callbackDeadlineBudgetNanoseconds),
        static_cast<jlong>(stats.callbackDeadlineMisses),
    };
    jlongArray out = env->NewLongArray(kCount);
    if (out) env->SetLongArrayRegion(out, 0, kCount, values);
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
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIsEngineError(JNIEnv* env, jobject thiz) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->hasError()
        ? JNI_TRUE : JNI_FALSE;
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

    jstring unitStr = env->NewStringUTF(port.unit.c_str());
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
        scalePointsList,
        unitStr,
        static_cast<jint>(port.stepCount),
        static_cast<jboolean>(port.isReadOnly ? JNI_TRUE : JNI_FALSE)
    );
    env->DeleteLocalRef(unitStr);

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
    setString(g_jni.piOriginPath, info.originPath);
    setString(g_jni.piModguiBasePath, info.modguiBasePath);
    setString(g_jni.piModguiIconTemplate, info.modguiIconTemplate);
    setString(g_jni.piX11UiBinaryPath, info.x11UiBinaryPath);
    setString(g_jni.piX11UiUri, info.x11UiUri);

    if (g_jni.piHasX11Ui)
        env->SetBooleanField(obj, g_jni.piHasX11Ui, info.hasX11Ui ? JNI_TRUE : JNI_FALSE);
    if (g_jni.piParameterMetadataRevision)
        env->SetLongField(obj, g_jni.piParameterMetadataRevision,
                          static_cast<jlong>(info.parameterMetadataRevision));

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
    if (!chain || position < 0 || static_cast<size_t>(position) >= chain->getSize()) return JNI_FALSE;
    auto managerIt = g_ctx->pluginUIManagers.find(pathId);
    if (managerIt != g_ctx->pluginUIManagers.end()) {
        managerIt->second->destroyPluginUI(position);
        managerIt->second->detachAndShiftForRemoval(position);
    }
    return chain->removePlugin(position) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeReorderRack(
    JNIEnv*, jobject, jlong pathId, jint fromPos, jint toPos) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    if (!chain) return JNI_FALSE;
    const bool ok = chain->reorderPlugins(fromPos, toPos);
    if (ok) {
        auto managerIt = g_ctx->pluginUIManagers.find(pathId);
        if (managerIt != g_ctx->pluginUIManagers.end())
            managerIt->second->reorderUIs(fromPos, toPos);
    }
    return ok ? JNI_TRUE : JNI_FALSE;
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
    JNIEnv*, jobject, jlong pathId, jlong pluginInstanceId, jint portIndex, jfloat value) {
    if (!g_ctx || !g_ctx->audioEngine || pluginInstanceId == 0 || portIndex < 0) return;
    std::lock_guard lock(g_ctx->rackControlMutex);
    if (auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId)))
        chain->submitParameter(static_cast<uint64_t>(pluginInstanceId),
                               static_cast<uint32_t>(portIndex), value);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetManualLatencyFrames(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex, jint frames) {
    if (!g_ctx || !g_ctx->audioEngine || frames < 0) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().setManualLatencyFrames(
        static_cast<RackPathId>(pathId), pluginIndex, static_cast<uint32_t>(frames))
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetManualLatencyFrames(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return static_cast<jint>(g_ctx->audioEngine->getRackGraph().getManualLatencyFrames(
        static_cast<RackPathId>(pathId), pluginIndex));
}
JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetManualLatencyRemainingFrames(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return static_cast<jint>(g_ctx->audioEngine->getRackGraph().getManualLatencyRemainingFrames(
        static_cast<RackPathId>(pathId), pluginIndex));
}
JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetPluginLatencyFrames(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return static_cast<jlong>(g_ctx->audioEngine->getRackGraph().getPluginLatencyFrames(
        static_cast<RackPathId>(pathId), pluginIndex));
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetPluginEffectiveLatencyFrames(
    JNIEnv*, jobject, jlong pathId, jint pluginIndex) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return static_cast<jlong>(g_ctx->audioEngine->getRackGraph().getPluginEffectiveLatencyFrames(
        static_cast<RackPathId>(pathId), pluginIndex));
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeHasPluginLatencyOverflow(
    JNIEnv*, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().hasPluginLatencyOverflow(
        static_cast<RackPathId>(pathId)) ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jfloat JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetParameter(
    JNIEnv*, jobject, jlong pathId, jlong pluginInstanceId, jint portIndex) {
    if (!g_ctx || !g_ctx->audioEngine || pluginInstanceId == 0 || portIndex < 0) return 0.0f;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    return chain ? chain->getParameter(static_cast<uint64_t>(pluginInstanceId),
                                        static_cast<uint32_t>(portIndex)) : 0.0f;
}

JNIEXPORT jfloatArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetParameterSnapshot(
    JNIEnv* env, jobject, jlong pathId, jlong pluginInstanceId, jintArray portIndices) {
    if (!g_ctx || !g_ctx->audioEngine || pluginInstanceId == 0 || !portIndices) return nullptr;
    const jsize count = env->GetArrayLength(portIndices);
    if (count < 0 || count > 4096) return nullptr;
    std::vector<jint> javaPorts(static_cast<size_t>(count));
    env->GetIntArrayRegion(portIndices, 0, count, javaPorts.data());
    if (env->ExceptionCheck()) return nullptr;
    std::vector<uint32_t> ports(static_cast<size_t>(count));
    for (jsize index = 0; index < count; ++index) {
        if (javaPorts[static_cast<size_t>(index)] < 0) return nullptr;
        ports[static_cast<size_t>(index)] =
            static_cast<uint32_t>(javaPorts[static_cast<size_t>(index)]);
    }
    std::shared_ptr<PluginChain> chain;
    {
        std::lock_guard lock(g_ctx->rackControlMutex);
        chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    }
    std::vector<float> values(static_cast<size_t>(count));
    if (!chain || !chain->getParameters(
            static_cast<uint64_t>(pluginInstanceId), ports.data(), ports.size(), values.data())) {
        return nullptr;
    }
    jfloatArray result = env->NewFloatArray(count);
    if (result) env->SetFloatArrayRegion(result, 0, count, values.data());
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetParameterDisplay(
    JNIEnv* env, jobject, jlong pathId, jlong pluginInstanceId, jint portIndex) {
    if (!g_ctx || !g_ctx->audioEngine || pluginInstanceId == 0 || portIndex < 0)
        return env->NewStringUTF("");
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(static_cast<RackPathId>(pathId));
    if (!chain) return env->NewStringUTF("");
    const std::string display = chain->getParameterDisplay(
        static_cast<uint64_t>(pluginInstanceId), static_cast<uint32_t>(portIndex));
    return env->NewStringUTF(display.c_str());
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
    auto& graph = g_ctx->audioEngine->getRackGraph();
    // Keep the target chain alive across publication and synchronous UI teardown.
    [[maybe_unused]] const auto removedChain =
        graph.getChain(static_cast<RackPathId>(trackId));
    // Keep the UI manager unchanged on any rejected mutation.
    if (!graph.removeTrack(trackId)) return JNI_FALSE;
    auto pathIt = g_ctx->pluginUIManagers.find(trackId);
    if (pathIt != g_ctx->pluginUIManagers.end()) {
        pathIt->second->rebindChain(nullptr);
        g_ctx->pluginUIManagers.erase(pathIt);
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackName(
    JNIEnv* env, jobject, jlong trackId, jstring value) {
    if (!g_ctx || !g_ctx->audioEngine || !value) return JNI_FALSE;
    const jsize length = env->GetStringLength(value);
    const jchar* utf16 = env->GetStringChars(value, nullptr);
    if (!utf16) return JNI_FALSE;
    size_t codePoints = 0;
    bool validUtf16 = length > 0;
    for (jsize i = 0; validUtf16 && i < length; ++i) {
        const jchar c = utf16[i];
        if (c == 0) {
            validUtf16 = false;
        } else if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 >= length || utf16[i + 1] < 0xdc00 || utf16[i + 1] > 0xdfff) {
                validUtf16 = false;
            } else {
                ++i;
            }
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            validUtf16 = false;
        }
        if (validUtf16 && (c < 0xdc00 || c > 0xdfff)) ++codePoints;
    }
    env->ReleaseStringChars(value, utf16);
    if (!validUtf16 || codePoints > 48) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return JNI_FALSE;
    const jsize utf8Length = env->GetStringUTFLength(value);
    const std::string name(chars, static_cast<size_t>(utf8Length));
    env->ReleaseStringUTFChars(value, chars);
    if (name.size() > 288 || !isValidTrackName(name)) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().setTrackName(
        static_cast<RackPathId>(trackId), name) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackColor(
    JNIEnv*, jobject, jlong trackId, jint argb) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().setTrackColor(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(argb)) ? JNI_TRUE : JNI_FALSE;
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
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackInputArmLocked(
    JNIEnv*, jobject, jlong trackId, jboolean locked) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputArmLocked(
        static_cast<RackPathId>(trackId), locked == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeArmTrackExclusively(
    JNIEnv*, jobject, jlong trackId) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputArmedExclusive(
        static_cast<RackPathId>(trackId)) ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackInputHardwarePair(
    JNIEnv*, jobject, jlong trackId, jint firstChannel) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputHardwarePair(
        static_cast<RackPathId>(trackId), static_cast<int32_t>(firstChannel)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackInputHardwareMono(
    JNIEnv*, jobject, jlong trackId, jint channel) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputHardwareMono(
        static_cast<RackPathId>(trackId), static_cast<int32_t>(channel)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackInputTrack(
    JNIEnv*, jobject, jlong trackId, jlong sourceTrackId, jint tap) {
    if (!g_ctx || !g_ctx->audioEngine || tap < 0 || tap > 1) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackInputTrack(
        static_cast<RackPathId>(trackId), static_cast<RackPathId>(sourceTrackId),
        static_cast<TrackInputTap>(tap)) ? JNI_TRUE : JNI_FALSE;
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
Java_com_vibes_dsp_engine_NativeEngine_nativeLoadTrackMidi(JNIEnv* env, jobject, jlong trackId, jstring path, jstring displayName) {
    if (!g_ctx || !g_ctx->audioEngine || !path || !displayName) return JNI_FALSE;
    const char* p=env->GetStringUTFChars(path,nullptr); const char* n=env->GetStringUTFChars(displayName,nullptr);
    std::shared_ptr<MidiClip> clip; const bool parsed=p&&n&&parseMidiFile(p,n,clip);
    if (p) env->ReleaseStringUTFChars(path,p); if (n) env->ReleaseStringUTFChars(displayName,n);
    return parsed && g_ctx->audioEngine->getRackGraph().attachTrackMidi(static_cast<RackPathId>(trackId),std::move(clip)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_vibes_dsp_engine_NativeEngine_nativeLoadTrackClipWav(JNIEnv* env,jobject,jlong id,jint slot,jstring path,jstring name,jdouble sourceBpm){if(!g_ctx||!g_ctx->audioEngine||slot<0||!path||!name||sourceBpm<=0.0)return JNI_FALSE;const char*p=env->GetStringUTFChars(path,nullptr);const char*n=env->GetStringUTFChars(name,nullptr);bool ok=p&&n&&g_ctx->audioEngine->loadTrackClipWav(id,static_cast<uint32_t>(slot),p,n,sourceBpm);if(p)env->ReleaseStringUTFChars(path,p);if(n)env->ReleaseStringUTFChars(name,n);return ok?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_com_vibes_dsp_engine_NativeEngine_nativeLoadTrackClipMidi(JNIEnv* env,jobject,jlong id,jint slot,jstring path,jstring name){if(!g_ctx||!g_ctx->audioEngine||slot<0||!path||!name)return JNI_FALSE;const char*p=env->GetStringUTFChars(path,nullptr);const char*n=env->GetStringUTFChars(name,nullptr);std::shared_ptr<MidiClip> c;bool ok=p&&n&&parseMidiFile(p,n,c)&&g_ctx->audioEngine->getRackGraph().attachTrackMidiSlot(id,static_cast<uint32_t>(slot),std::move(c));if(p)env->ReleaseStringUTFChars(path,p);if(n)env->ReleaseStringUTFChars(name,n);return ok?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_com_vibes_dsp_engine_NativeEngine_nativeSelectTrackClipSlot(JNIEnv*,jobject,jlong id,jint slot){return g_ctx&&g_ctx->audioEngine&&slot>=0&&g_ctx->audioEngine->selectTrackClipSlot(id,static_cast<uint32_t>(slot))?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipSourceBpm(
    JNIEnv*, jobject, jlong trackId, jint slot, jdouble sourceBpm) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0 ||
        !std::isfinite(static_cast<double>(sourceBpm)) ||
        sourceBpm < 20.0 || sourceBpm > 400.0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setClipSourceBpm(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<double>(sourceBpm)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipTempoMode(JNIEnv*,jobject,jlong id,jint slot,jint mode){if(!g_ctx||!g_ctx->audioEngine||slot<0||mode<0||mode>2)return JNI_FALSE;return g_ctx->audioEngine->getRackGraph().setClipTempoMode(static_cast<RackPathId>(id),static_cast<uint32_t>(slot),static_cast<ClipTempoMode>(mode))?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_com_vibes_dsp_engine_NativeEngine_nativeRenameTrackClip(JNIEnv* env,jobject,jlong trackId,jint slot,jstring displayName){if(!g_ctx||!g_ctx->audioEngine||slot<0||!displayName)return JNI_FALSE;const char* name=env->GetStringUTFChars(displayName,nullptr);if(!name)return JNI_FALSE;const bool ok=g_ctx->audioEngine->getRackGraph().renameTrackClip(static_cast<RackPathId>(trackId),slot,name);env->ReleaseStringUTFChars(displayName,name);return ok?JNI_TRUE:JNI_FALSE;}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTrackClipSlots(
    JNIEnv* env, jobject, jlong trackId) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    const auto slots = g_ctx->audioEngine->getRackGraph().getTrackClipSlots(
        static_cast<RackPathId>(trackId));
    jclass clazz = env->FindClass("com/vibes/dsp/engine/ClipSlotInfo");
    if (!clazz) return nullptr;
    jmethodID ctor = env->GetMethodID(
        clazz, "<init>", "(JIZZLjava/lang/String;DZZZDJDZDIDZDJDD)V");
    if (!ctor) {
        env->DeleteLocalRef(clazz);
        return nullptr;
    }
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(slots.size()), clazz, nullptr);
    for (size_t index = 0; result && index < slots.size(); ++index) {
        const auto& slot = slots[index];
        jstring name = env->NewStringUTF(slot.displayName.c_str());
        jobject item = env->NewObject(
            clazz, ctor, static_cast<jlong>(slot.trackId), static_cast<jint>(slot.slot),
            slot.wavLoaded ? JNI_TRUE : JNI_FALSE, slot.midiLoaded ? JNI_TRUE : JNI_FALSE,
            name, slot.durationSec, slot.active ? JNI_TRUE : JNI_FALSE,
            slot.playing ? JNI_TRUE : JNI_FALSE, slot.looping ? JNI_TRUE : JNI_FALSE,
            slot.positionSec, static_cast<jlong>(slot.transportFrame),
            slot.loopLengthBars, slot.enterOnPunch ? JNI_TRUE : JNI_FALSE,
            slot.sourceBpm, static_cast<jint>(slot.tempoMode), slot.defaultLoopLengthBars,
            slot.launchPending ? JNI_TRUE : JNI_FALSE,
            slot.musicalQuarterNotes,
            static_cast<jlong>(slot.capturedAtMonotonicNanos),
            slot.loopStartQuarterNotes, slot.loopLengthQuarterNotes);
        if (item) env->SetObjectArrayElement(result, static_cast<jsize>(index), item);
        if (name) env->DeleteLocalRef(name);
        if (item) env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(clazz);
    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTrackClipMidiNotes(
    JNIEnv* env, jobject, jlong trackId, jint slot) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return nullptr;
    const auto notes = g_ctx->audioEngine->getRackGraph().getTrackClipMidiNotes(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot));
    jclass clazz = env->FindClass("com/vibes/dsp/engine/MidiNoteInfo");
    if (!clazz) return nullptr;
    jmethodID ctor = env->GetMethodID(clazz, "<init>", "(JJII)V");
    if (!ctor) return nullptr;
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(notes.size()), clazz, nullptr);
    for (size_t index = 0; index < notes.size(); ++index) {
        const auto& note = notes[index];
        jobject item = env->NewObject(
            clazz, ctor, static_cast<jlong>(note.startMicroseconds),
            static_cast<jlong>(note.durationMicroseconds), static_cast<jint>(note.pitch),
            static_cast<jint>(note.velocity));
        env->SetObjectArrayElement(result, static_cast<jsize>(index), item);
        env->DeleteLocalRef(item);
    }
    return result;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeUnloadTrackMidi(JNIEnv*, jobject, jlong trackId) {
    return g_ctx&&g_ctx->audioEngine&&g_ctx->audioEngine->getRackGraph().unloadTrackMidi(static_cast<RackPathId>(trackId))?JNI_TRUE:JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeUnloadTrackWav(JNIEnv*, jobject, jlong trackId) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->unloadTrackWav(trackId) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeUnloadTrackClipMidi(
    JNIEnv*, jobject, jlong trackId, jint slot) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().unloadTrackMidiSlot(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot))
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeUnloadTrackClipWav(
    JNIEnv*, jobject, jlong trackId, jint slot) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().unloadTrackWavSlot(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot))
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeClearTrackWavs(JNIEnv*, jobject) {
    return g_ctx && g_ctx->audioEngine && g_ctx->audioEngine->getRackGraph().clearTrackWavs() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTrackWaveformPeaks(
    JNIEnv* env, jobject, jlong trackId, jint maxBuckets) {
    if (!g_ctx || !g_ctx->audioEngine || maxBuckets <= 0) return env->NewFloatArray(0);
    const auto peaks = g_ctx->audioEngine->getRackGraph().getTrackWaveformPeaks(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(maxBuckets));
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(peaks.size()));
    if (result && !peaks.empty()) {
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(peaks.size()), peaks.data());
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTransportPlaying(JNIEnv*, jobject, jboolean playing) {
    return g_ctx && g_ctx->audioEngine &&
        g_ctx->audioEngine->getRackGraph().setTransportPlaying(playing == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeRestartTransport(JNIEnv*, jobject) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().restartTransport() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStopTransport(JNIEnv*, jobject) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    g_ctx->audioEngine->getRackGraph().pauseAndResetTransport();
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTransportBpm(JNIEnv*, jobject, jdouble bpm) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    g_ctx->audioEngine->getRackGraph().setBeatsPerMinute(bpm);
    return JNI_TRUE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackDefaultLoopLength(
    JNIEnv*, jobject, jlong trackId, jdouble bars) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setTrackDefaultLoopLength(
        static_cast<RackPathId>(trackId), static_cast<double>(bars)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetSlotDefaultLoopLength(
    JNIEnv*, jobject, jlong trackId, jint slot, jdouble bars) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setSlotDefaultLoopLength(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<double>(bars)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipLoopLength(
    JNIEnv*, jobject, jlong trackId, jint slot, jdouble bars) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setClipLoopLength(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<double>(bars)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipLoopStartQuarterNotes(
    JNIEnv*, jobject, jlong trackId, jint slot, jdouble value) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setClipLoopStartQuarterNotes(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<double>(value)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipLoopLengthQuarterNotes(
    JNIEnv*, jobject, jlong trackId, jint slot, jdouble value) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setClipLoopLengthQuarterNotes(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<double>(value)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipLooping(
    JNIEnv*, jobject, jlong trackId, jint slot, jboolean looping) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0) return JNI_FALSE;
    return g_ctx->audioEngine->getRackGraph().setClipLooping(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        looping == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetSlotEnterOnPunch(
    JNIEnv*, jobject, jlong trackId, jint slot, jboolean armed, jint quantization) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0 || quantization < 0 || quantization > 4) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->getRackGraph().setSlotEnterOnPunch(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        armed == JNI_TRUE, static_cast<LaunchQuantization>(quantization)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetClipTransportPlaying(
    JNIEnv*, jobject, jlong trackId, jint slot, jboolean playing, jint quantization) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0 || quantization < 0 || quantization > 4) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->getRackGraph().setClipTransportPlaying(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        playing == JNI_TRUE, static_cast<LaunchQuantization>(quantization)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeStartTrackClipRecording(
    JNIEnv*, jobject, jlong trackId, jint slot, jint quantization) {
    if (!g_ctx || !g_ctx->audioEngine || slot < 0 || quantization < 0 || quantization > 4) {
        return JNI_FALSE;
    }
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().startTrackClipRecording(
        static_cast<RackPathId>(trackId), static_cast<uint32_t>(slot),
        static_cast<LaunchQuantization>(quantization)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeCancelTrackLoopRecording(
    JNIEnv*, jobject, jlong trackId) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    return g_ctx->audioEngine->getRackGraph().cancelTrackLoopRecording(
        static_cast<RackPathId>(trackId)) ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackSize(JNIEnv*, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return 0;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    return chain ? static_cast<jint>(chain->getSize()) : 0;
}

JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackRealtimeDiagnostic(
    JNIEnv* env, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return env->NewStringUTF("engine-unavailable");
    std::shared_ptr<PluginChain> chain;
    {
        std::lock_guard lock(g_ctx->rackControlMutex);
        chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    }
    const std::string diagnostic =
        chain ? chain->getRealtimeDiagnostic() : "path-not-found";
    return env->NewStringUTF(diagnostic.c_str());
}

JNIEXPORT jobject JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetRackPluginInfo(JNIEnv* env, jobject, jlong pathId, jint index) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    if (!chain || index < 0 || static_cast<size_t>(index) >= chain->getSize()) return nullptr;
    const PluginInfo info = chain->visitPlugin(
            static_cast<size_t>(index), [](const IPlugin& plugin) { return plugin.getInfo(); });
    return createPluginInfoObject(env, info);
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
        const PluginInfo pluginInfo = chain->visitPlugin(
                index, [](const IPlugin& plugin) { return plugin.getInfo(); });
        jobject info = createPluginInfoObject(env, pluginInfo);
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
    jmethodID ctor = env->GetMethodID(
        clazz, "<init>", "(JFZZZLjava/lang/String;DZZDJZZZIIJIZZIDIDDJILjava/lang/String;I)V");
    if (!ctor) {
        env->DeleteLocalRef(clazz);
        return nullptr;
    }
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(tracks.size()), clazz, nullptr);
    for (size_t index = 0; result && index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        jstring name = env->NewStringUTF(track.wavDisplayName.c_str());
        jstring trackName = env->NewStringUTF(track.name.c_str());
        jobject item = env->NewObject(
            clazz, ctor, static_cast<jlong>(track.id), track.volume,
            track.inputArmed ? JNI_TRUE : JNI_FALSE,
            track.inputArmLocked ? JNI_TRUE : JNI_FALSE,
            track.wavLoaded ? JNI_TRUE : JNI_FALSE, name, track.wavDurationSec,
            track.playing ? JNI_TRUE : JNI_FALSE, track.looping ? JNI_TRUE : JNI_FALSE,
            track.positionSec, static_cast<jlong>(track.transportFrame),
            track.recordPending ? JNI_TRUE : JNI_FALSE,
            track.recording ? JNI_TRUE : JNI_FALSE,
            track.punchArmed ? JNI_TRUE : JNI_FALSE,
            static_cast<jint>(track.inputSourceKind),
            static_cast<jint>(track.inputSourceFirstChannel),
            static_cast<jlong>(track.inputSourceTrackId),
            static_cast<jint>(track.inputTap),
            track.midiLoaded ? JNI_TRUE : JNI_FALSE,
            track.midiPlaying ? JNI_TRUE : JNI_FALSE,
            static_cast<jint>(track.selectedSlot),
            track.defaultLoopLengthBars,
            static_cast<jint>(track.activeSlot),
            track.musicalQuarterNotes, track.sampleRate,
            static_cast<jlong>(track.capturedAtMonotonicNanos),
            static_cast<jint>(track.recordingSlot), trackName,
            static_cast<jint>(track.colorArgb));
        if (item) env->SetObjectArrayElement(result, static_cast<jsize>(index), item);
        if (name) env->DeleteLocalRef(name);
        if (trackName) env->DeleteLocalRef(trackName);
        if (item) env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(clazz);
    return result;
}

JNIEXPORT jobject JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetTransportInfo(JNIEnv* env, jobject) {
    const TransportSnapshot state = g_ctx && g_ctx->audioEngine
        ? g_ctx->audioEngine->getRackGraph().getTransportSnapshot() : TransportSnapshot{};
    jclass clazz = env->FindClass("com/vibes/dsp/engine/TransportInfo");
    if (!clazz) return nullptr;
    jmethodID ctor = env->GetMethodID(clazz, "<init>", "(ZDDJJDDJ)V");
    return ctor ? env->NewObject(clazz, ctor, state.playing ? JNI_TRUE : JNI_FALSE,
                                 state.positionSec, state.beatsPerMinute,
                                 static_cast<jlong>(state.samplePosition),
                                 static_cast<jlong>(state.transportFrame),
                                 state.musicalQuarterNotes, state.sampleRate,
                                 static_cast<jlong>(state.capturedAtMonotonicNanos)) : nullptr;
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
    std::unique_lock<std::mutex> rackLock;
    PluginUIManager* manager = nullptr;
    if (!g_ctx || !g_ctx->audioEngine) {
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
        setCreatingPluginUI(false);
        return JNI_FALSE;
    }
    // Fetch and validate the chain while holding the same lock used by rack
    // mutations; otherwise a replacement can invalidate the selected slot
    // between validation and UI-manager binding.
    rackLock = std::unique_lock<std::mutex>(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    if (chain && pluginIndex >= 0 &&
        static_cast<size_t>(pluginIndex) < chain->getSize() &&
        static_cast<jlong>(chain->getPluginInstanceId(pluginIndex)) == pluginInstanceId) {
        manager = getPluginUIManagerLocked(*g_ctx, pathId, chain.get());
    }
    if (!manager) {
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
        setCreatingPluginUI(false);
        return JNI_FALSE;
    }
    bool result = manager->createPluginUI(
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
        LOGI("nativeCreatePluginUI: deferred detach detected, destroying plugin UI");
        manager->destroyPluginUI(pluginIndex);
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
    if (!g_ctx || !g_ctx->audioEngine) return;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    auto managerIt = g_ctx->pluginUIManagers.find(pathId);
    if (!chain || managerIt == g_ctx->pluginUIManagers.end()) return;
    const int pluginIndex = [&]() {
        for (size_t i = 0; i < chain->getSize(); ++i)
            if (static_cast<jlong>(chain->getPluginInstanceId(static_cast<int>(i))) == pluginInstanceId)
                return static_cast<int>(i);
        return -1;
    }();
    if (pluginIndex >= 0) managerIt->second->destroyPluginUI(pluginIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeIdlePluginUIs(JNIEnv*, jobject) {
    if (!g_ctx || !g_ctx->audioEngine) return JNI_FALSE;
    std::lock_guard lock(g_ctx->rackControlMutex);
    bool result = false;
    for (auto& entry : g_ctx->pluginUIManagers) {
        result = entry.second->idleAllUIs() || result;
    }
    return result ? JNI_TRUE : JNI_FALSE;
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
Java_com_vibes_dsp_engine_NativeEngine_nativeGetX11UIScale(JNIEnv*, jobject, jint displayNumber) {
    return withDisplayGetUIScale(displayNumber);
}

JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativePollFileRequest(JNIEnv* env, jobject, jlong pathId) {
    if (!g_ctx || !g_ctx->audioEngine) return nullptr;
    guitarrackcraft::PluginUIManager::FileRequest req;
    {
        std::lock_guard lock(g_ctx->rackControlMutex);
        auto it = g_ctx->pluginUIManagers.find(pathId);
        if (it == g_ctx->pluginUIManagers.end() || !it->second->pollFileRequest(req)) {
            return nullptr;
        }
    }


    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(3, stringClass, nullptr);
    if (!result) return nullptr;

    jstring pathStr = env->NewStringUTF(std::to_string(req.pathId).c_str());
    jstring indexStr = env->NewStringUTF(std::to_string(req.pluginIndex).c_str());
    jstring uriStr = env->NewStringUTF(req.propertyUri.c_str());
    env->SetObjectArrayElement(result, 0, pathStr);
    env->SetObjectArrayElement(result, 1, indexStr);
    env->SetObjectArrayElement(result, 2, uriStr);
    env->DeleteLocalRef(pathStr);
    env->DeleteLocalRef(indexStr);
    env->DeleteLocalRef(uriStr);
    env->DeleteLocalRef(stringClass);

    return result;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeDeliverFileToPluginUI(
    JNIEnv* env, jobject, jlong pathId, jint pluginIndex, jstring propertyUri, jstring filePath)
{
    if (!g_ctx || !g_ctx->audioEngine || !propertyUri || !filePath) return;
    std::lock_guard lock(g_ctx->rackControlMutex);
    auto chain = g_ctx->audioEngine->getRackGraph().getChain(pathId);
    auto managerIt = g_ctx->pluginUIManagers.find(pathId);
    if (!chain || managerIt == g_ctx->pluginUIManagers.end() || pluginIndex < 0 ||
        static_cast<size_t>(pluginIndex) >= chain->getSize()) return;

    const char* propStr = env->GetStringUTFChars(propertyUri, nullptr);
    const char* pathStr = env->GetStringUTFChars(filePath, nullptr);
    if (propStr && pathStr) {
        managerIt->second->deliverFileToUI(
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
    if (!chain || pluginIndex < 0 || static_cast<size_t>(pluginIndex) >= chain->getSize()) {
        return nullptr;
    }
    NativeFilePickerRequest req;
    const bool hasRequest = chain->visitPlugin(
            static_cast<size_t>(pluginIndex),
            [&](IPlugin& plugin) { return plugin.pollNativeFilePicker(req); });
    if (!hasRequest) return nullptr;

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
    if (!chain || pluginIndex < 0 || static_cast<size_t>(pluginIndex) >= chain->getSize()) return;

    std::string path;
    if (windowsPath) {
        const char* pathStr = env->GetStringUTFChars(windowsPath, nullptr);
        if (pathStr) {
            path = pathStr;
            env->ReleaseStringUTFChars(windowsPath, pathStr);
        }
    }

    chain->visitPlugin(static_cast<size_t>(pluginIndex), [&](IPlugin& plugin) {
        plugin.respondNativeFilePicker(
            static_cast<uint32_t>(sequence),
            cancelled == JNI_TRUE,
            path);
    });
}




} // extern "C"
