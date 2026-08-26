/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "NativeContextAccess.h"
#include "../jsfx/IJsfxUiTarget.h"
#include "../jsfx/JsfxUiHost.h"
#include "../plugin/PluginChain.h"
#include "../plugin/RackGraph.h"

#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace guitarrackcraft;

namespace {

template <class Callback>
bool visit(jlong path, jlong pluginInstanceId, Callback&& callback) {
    if (path < 0 || pluginInstanceId <= 0) return false;
    auto* graph = nativeRackGraph();
    if (!graph) return false;
    auto chain = graph->getChain(static_cast<RackPathId>(path));
    if (!chain) return false;
    bool handled = false;
    const bool found = chain->visitPluginInstance(
        static_cast<uint64_t>(pluginInstanceId), [&](IPlugin& plugin) {
            if (auto* target = dynamic_cast<IJsfxUiTarget*>(&plugin)) {
                handled = callback(*target);
            }
        });
    return found && handled;
}

std::string fromJString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}

void appendJsonString(std::string& output, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[character >> 4]);
                    output.push_back(hex[character & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output.push_back('"');
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeHasGfx(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId) {
    return visit(path, pluginInstanceId, [](IJsfxUiTarget& target) {
        return target.hasJsfxGfx();
    }) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeAttach(
        JNIEnv* env, jobject, jlong path, jlong pluginInstanceId,
        jobject surface, jint width, jint height) {
    if (!surface || width <= 0 || height <= 0) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    const bool attached = visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->attachWindow(window);
        host->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        return true;
    });
    ANativeWindow_release(window);
    return attached ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeDetach(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId) {
    visit(path, pluginInstanceId, [](IJsfxUiTarget& target) {
        if (auto* host = target.jsfxUiHost()) host->detachWindow();
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeResize(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jint width, jint height) {
    if (width <= 0 || height <= 0) return;
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeSetVisible(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jboolean visible) {
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->setVisible(visible == JNI_TRUE);
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeSetFocus(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jboolean focused) {
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->setFocus(focused == JNI_TRUE);
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativePointer(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jint action, jint,
        jfloat x, jfloat y, jint buttons, jfloat horizontalWheel, jfloat verticalWheel,
        jint modifiers) {
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->setMouseOver(action != 10); // MotionEvent.ACTION_HOVER_EXIT
        host->pointer(
            static_cast<uint32_t>(modifiers), static_cast<int32_t>(x), static_cast<int32_t>(y),
            static_cast<uint32_t>(buttons), static_cast<double>(verticalWheel),
            static_cast<double>(horizontalWheel));
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeKey(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jboolean down,
        jint key, jint unicode, jint modifiers, jint) {
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->key(
            static_cast<uint32_t>(modifiers),
            static_cast<uint32_t>(key != 0 ? key : unicode),
            down == JNI_TRUE);
        return true;
    });
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeSetDropFiles(
        JNIEnv* env, jobject, jlong path, jlong pluginInstanceId, jobjectArray paths) {
    std::vector<std::string> files;
    if (paths) {
        const jsize count = env->GetArrayLength(paths);
        files.reserve(static_cast<size_t>(count));
        for (jsize index = 0; index < count; ++index) {
            auto value = static_cast<jstring>(env->GetObjectArrayElement(paths, index));
            files.push_back(fromJString(env, value));
            env->DeleteLocalRef(value);
        }
    }
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->setDropFiles(files);
        return true;
    });
}

JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativePollMenu(
        JNIEnv* env, jobject, jlong path, jlong pluginInstanceId) {
    JsfxUiHost::MenuRequest request{};
    const bool available = visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        return host && host->pollMenu(request);
    });
    if (!available) return nullptr;
    std::string json = "{\"id\":" + std::to_string(request.id) + ",\"spec\":";
    appendJsonString(json, request.spec);
    json += ",\"x\":" + std::to_string(request.x) +
            ",\"y\":" + std::to_string(request.y) + "}";
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT void JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeRespondMenu(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId, jlong requestId, jint itemId) {
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->respondMenu(static_cast<uint64_t>(requestId), itemId);
        return true;
    });
}

JNIEXPORT jint JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeGetCursor(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId) {
    int32_t cursor = 0;
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        cursor = host->cursor();
        return true;
    });
    return cursor;
}

JNIEXPORT jlong JNICALL
Java_com_vibes_dsp_engine_JsfxBridge_nativeGetPreferredSize(
        JNIEnv*, jobject, jlong path, jlong pluginInstanceId) {
    uint32_t width = 0;
    uint32_t height = 0;
    visit(path, pluginInstanceId, [&](IJsfxUiTarget& target) {
        auto* host = target.jsfxUiHost();
        if (!host) return false;
        host->preferredSize(width, height);
        return true;
    });
    return (static_cast<jlong>(width) << 32) | height;
}

} // extern "C"
