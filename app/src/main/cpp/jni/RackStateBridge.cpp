/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA. NNAGA is free software under GPLv3+.
 */
#include "NativeContextAccess.h"
#include "../plugin/RackStateCodec.h"
#include <jni.h>
#include <mutex>
#include <string>
#include <vector>
using guitarrackcraft::RackStateCodec;
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeExportRackState(JNIEnv* env, jobject) {
    auto* graph=guitarrackcraft::nativeRackGraph();
    auto* mutex=guitarrackcraft::nativeRackMutex();
    if (!graph || !mutex) return nullptr;
    std::lock_guard<std::mutex> lock(*mutex);
    std::string error;
    const auto bytes=RackStateCodec::encode(graph->saveState(), &error);
    if (bytes.empty()) return nullptr;
    auto out=env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!out) return nullptr;
    env->SetByteArrayRegion(out,0,static_cast<jsize>(bytes.size()),reinterpret_cast<const jbyte*>(bytes.data()));
    return out;
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeImportRackState(
        JNIEnv* env, jobject, jbyteArray input, jboolean restorePlugins) {
    if (!input) return env->NewStringUTF("invalid-input:null");
    auto* graph=guitarrackcraft::nativeRackGraph(); auto* registry=guitarrackcraft::nativePluginRegistry();
    auto* mutex=guitarrackcraft::nativeRackMutex();
    if (!graph || !registry || !mutex) return env->NewStringUTF("engine-unavailable");
    const jsize size=env->GetArrayLength(input);
    if (size<=0 || static_cast<size_t>(size)>RackStateCodec::kMaxBlobBytes) return env->NewStringUTF("invalid-size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    env->GetByteArrayRegion(input,0,size,reinterpret_cast<jbyte*>(bytes.data()));
    guitarrackcraft::RackGraph::State state; std::string diagnostic;
    if (!RackStateCodec::decode(bytes.data(),bytes.size(),state,diagnostic)) return env->NewStringUTF(diagnostic.c_str());
    std::lock_guard<std::mutex> lock(*mutex);
    if (!graph->restoreState(state,*registry,diagnostic,restorePlugins == JNI_TRUE)) return env->NewStringUTF(diagnostic.c_str());
    return nullptr;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeExportDeviceChain(
        JNIEnv* env, jobject, jlong pathId) {
    auto* graph = guitarrackcraft::nativeRackGraph();
    auto* mutex = guitarrackcraft::nativeRackMutex();
    if (!graph || !mutex) return nullptr;
    guitarrackcraft::PluginChain::ChainState chain;
    std::string diagnostic;
    std::lock_guard<std::mutex> lock(*mutex);
    if (!graph->exportDeviceChain(static_cast<guitarrackcraft::RackPathId>(pathId), chain, diagnostic)) return nullptr;
    const auto bytes = RackStateCodec::encodeDeviceChain(static_cast<guitarrackcraft::RackPathId>(pathId), chain, &diagnostic);
    if (bytes.empty()) return nullptr;
    auto out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!out) return nullptr;
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeImportDeviceChain(
        JNIEnv* env, jobject, jlong pathId, jbyteArray input) {
    if (!input) return JNI_FALSE;
    auto* graph = guitarrackcraft::nativeRackGraph();
    auto* registry = guitarrackcraft::nativePluginRegistry();
    auto* mutex = guitarrackcraft::nativeRackMutex();
    if (!graph || !registry || !mutex) return JNI_FALSE;
    const jsize size = env->GetArrayLength(input);
    if (size <= 0 || static_cast<size_t>(size) > RackStateCodec::kMaxBlobBytes) return JNI_FALSE;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    env->GetByteArrayRegion(input, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    guitarrackcraft::RackPathId encodedPath = 0;
    guitarrackcraft::PluginChain::ChainState chain;
    std::string diagnostic;
    if (!RackStateCodec::decodeDeviceChain(bytes.data(), bytes.size(), encodedPath, chain, diagnostic) ||
        encodedPath != static_cast<guitarrackcraft::RackPathId>(pathId)) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(*mutex);
    const auto previous = graph->getChain(static_cast<guitarrackcraft::RackPathId>(pathId));
    (void)previous;
    if (!graph->importDeviceChain(static_cast<guitarrackcraft::RackPathId>(pathId), chain, *registry, diagnostic)) {
        return JNI_FALSE;
    }
    const auto replacement = graph->getChain(static_cast<guitarrackcraft::RackPathId>(pathId));
    guitarrackcraft::nativeRebindPluginUIManager(static_cast<int64_t>(pathId), replacement.get());
    return JNI_TRUE;
}
