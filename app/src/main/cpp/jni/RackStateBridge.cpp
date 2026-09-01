/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA. NNAGA is free software under GPLv3+.
 */
#include "NativeContextAccess.h"
#include "../plugin/RackStateCodec.h"
#include <jni.h>
#include <mutex>
#include <exception>
#include <string>
#include <vector>
using guitarrackcraft::RackStateCodec;
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeExportRackState(JNIEnv* env, jobject) {
    auto* graph=guitarrackcraft::nativeRackGraph();
    if (!graph) return nullptr;
    std::string error;
    const auto bytes=RackStateCodec::encode(graph->saveState(), &error);
    if (bytes.empty()) return nullptr;
    auto out=env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!out) return nullptr;
    env->SetByteArrayRegion(out,0,static_cast<jsize>(bytes.size()),reinterpret_cast<const jbyte*>(bytes.data()));
    return out;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetProjectStateSnapshot(JNIEnv* env, jobject) {
    auto* graph = guitarrackcraft::nativeRackGraph();
    if (!graph) return nullptr;
    std::string error;
    const auto state = graph->saveState();
    const auto bytes = RackStateCodec::encode(state, &error);
    if (bytes.empty()) return nullptr;

    jclass refClass = env->FindClass("com/vibes/dsp/engine/ProjectClipMediaRef");
    jclass snapshotClass = env->FindClass("com/vibes/dsp/engine/ProjectStateSnapshot");
    if (!refClass || !snapshotClass) return nullptr;
    const jmethodID refCtor = env->GetMethodID(refClass, "<init>", "(JILjava/lang/String;Z)V");
    const jmethodID snapshotCtor = env->GetMethodID(
        snapshotClass, "<init>", "([B[Lcom/vibes/dsp/engine/ProjectClipMediaRef;)V");
    if (!refCtor || !snapshotCtor) return nullptr;

    size_t refCount = 0;
    for (const auto& track : state.tracks) {
        for (const auto& clip : track.clipSlots) {
            if (!clip.assetId.empty()) ++refCount;
            if (!clip.midiAssetId.empty()) ++refCount;
        }
    }
    jobjectArray refs = env->NewObjectArray(static_cast<jsize>(refCount), refClass, nullptr);
    jbyteArray stateBytes = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!refs || !stateBytes) return nullptr;
    env->SetByteArrayRegion(stateBytes, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
    jsize index = 0;
    for (const auto& track : state.tracks) {
        for (const auto& clip : track.clipSlots) {
            const auto emit = [&](const std::string& asset, bool midi) {
                if (asset.empty()) return;
                jstring name = env->NewStringUTF(asset.c_str());
                jobject ref = env->NewObject(refClass, refCtor,
                    static_cast<jlong>(track.id), static_cast<jint>(clip.slot), name,
                    static_cast<jboolean>(midi));
                env->DeleteLocalRef(name);
                if (ref) {
                    env->SetObjectArrayElement(refs, index++, ref);
                    env->DeleteLocalRef(ref);
                }
            };
            emit(clip.assetId, false);
            emit(clip.midiAssetId, true);
        }
    }
    jobject result = env->NewObject(snapshotClass, snapshotCtor, stateBytes, refs);
    env->DeleteLocalRef(stateBytes);
    env->DeleteLocalRef(refs);
    return result;
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeImportRackState(
        JNIEnv* env, jobject, jbyteArray input, jboolean restorePlugins) {
    if (!input) return env->NewStringUTF("invalid-input:null");
    auto* graph = guitarrackcraft::nativeRackGraph();
    auto* registry = guitarrackcraft::nativePluginRegistry();
    auto* mutex = guitarrackcraft::nativeRackMutex();
    if (!graph || !registry || !mutex) {
        return env->NewStringUTF("engine-unavailable");
    }
    bool prepared = false;
    std::unique_lock<std::mutex> restoreLock;
    try {
        const jsize size = env->GetArrayLength(input);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        env->GetByteArrayRegion(
            input, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
        guitarrackcraft::RackGraph::State state;
        std::string diagnostic;
        if (!RackStateCodec::decode(
                bytes.data(), bytes.size(), state, diagnostic)) {
            return env->NewStringUTF(diagnostic.c_str());
        }
        restoreLock = std::unique_lock<std::mutex>(*mutex);
        guitarrackcraft::nativePrepareRackStateImport();
        prepared = true;
        if (!graph->restoreState(
                state, *registry, diagnostic, restorePlugins == JNI_TRUE)) {
            guitarrackcraft::nativeAbortRackStateImport();
            prepared = false;
            return env->NewStringUTF(diagnostic.c_str());
        }
        guitarrackcraft::nativeCommitRackStateImport();
        prepared = false;
        return nullptr;
    } catch (...) {
        if (prepared) {
            if (!restoreLock.owns_lock()) restoreLock.lock();
            guitarrackcraft::nativeAbortRackStateImport();
        }
        return env->NewStringUTF("restore-exception");
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeExportDeviceChain(
        JNIEnv* env, jobject, jlong pathId) {
    auto* graph = guitarrackcraft::nativeRackGraph();
    if (!graph) return nullptr;
    guitarrackcraft::PluginChain::ChainState chain;
    std::string diagnostic;
    if (!graph->exportDeviceChain(static_cast<guitarrackcraft::RackPathId>(pathId), chain, diagnostic)) return nullptr;
    const auto bytes = RackStateCodec::encodeDeviceChain(static_cast<guitarrackcraft::RackPathId>(pathId), chain, &diagnostic);
    if (bytes.empty()) return nullptr;
    auto out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!out) return nullptr;
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    return out;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeImportDeviceChain(
        JNIEnv* env, jobject, jlong pathId, jbyteArray input) {
    const auto diagnosticString = [&](const std::string& diagnostic) -> jstring {
        return env->NewStringUTF(diagnostic.c_str());
    };
    if (!input) return diagnosticString("invalid-input:null");
    auto* graph = guitarrackcraft::nativeRackGraph();
    auto* registry = guitarrackcraft::nativePluginRegistry();
    auto* mutex = guitarrackcraft::nativeRackMutex();
    if (!graph || !registry || !mutex) return diagnosticString("engine-unavailable");
    const jsize size = env->GetArrayLength(input);
    if (size <= 0 || static_cast<size_t>(size) > RackStateCodec::kMaxBlobBytes) {
        return diagnosticString("invalid-size");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    env->GetByteArrayRegion(input, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    guitarrackcraft::RackPathId encodedPath = 0;
    guitarrackcraft::PluginChain::ChainState chain;
    std::string diagnostic;
    if (!RackStateCodec::decodeDeviceChain(
            bytes.data(), bytes.size(), encodedPath, chain, diagnostic)) {
        return diagnosticString(diagnostic);
    }
    if (encodedPath != static_cast<guitarrackcraft::RackPathId>(pathId)) {
        return diagnosticString("path-mismatch");
    }
    std::lock_guard<std::mutex> lock(*mutex);
    const auto previous =
        graph->getChain(static_cast<guitarrackcraft::RackPathId>(pathId));
    if (!graph->importDeviceChain(
            static_cast<guitarrackcraft::RackPathId>(pathId),
            chain, *registry, diagnostic)) {
        return diagnosticString(diagnostic);
    }
    const auto replacement =
        graph->getChain(static_cast<guitarrackcraft::RackPathId>(pathId));
    guitarrackcraft::nativeRebindPluginUIManager(
        static_cast<int64_t>(pathId), replacement.get());
    return nullptr;
}
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeGetProjectClipMediaRefs(JNIEnv* env, jobject) {
    auto* graph = guitarrackcraft::nativeRackGraph();
    if (!graph) return nullptr;
    const auto refs = graph->getProjectClipMediaRefs();
    jclass cls = env->FindClass("com/vibes/dsp/engine/ProjectClipMediaRef");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JILjava/lang/String;Z)V");
    if (!ctor) return nullptr;
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(refs.size()), cls, nullptr);
    if (!result) return nullptr;
    for (jsize i = 0; i < static_cast<jsize>(refs.size()); ++i) {
        const auto& [track, slot, asset, midi] = refs[static_cast<size_t>(i)];
        jstring name = env->NewStringUTF(asset.c_str());
        jobject item = env->NewObject(cls, ctor, static_cast<jlong>(track),
                                      static_cast<jint>(slot), name,
                                      static_cast<jboolean>(midi));
        env->DeleteLocalRef(name);
        if (!item) return nullptr;
        env->SetObjectArrayElement(result, i, item);
        env->DeleteLocalRef(item);
    }
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeSetTrackClipAssetId(
        JNIEnv* env, jobject, jlong trackId, jint slot, jboolean isMidi, jstring assetId) {
    if (!assetId || slot < 0) return JNI_FALSE;
    auto* graph = guitarrackcraft::nativeRackGraph();
    if (!graph) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(assetId, nullptr);
    if (!chars) return JNI_FALSE;
    const bool ok = graph->setTrackClipAssetId(
        static_cast<guitarrackcraft::RackPathId>(trackId),
        static_cast<uint32_t>(slot), isMidi == JNI_TRUE, chars);
    env->ReleaseStringUTFChars(assetId, chars);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_vibes_dsp_engine_NativeEngine_nativeMaterializeProjectMedia(
        JNIEnv* env, jobject, jstring directory) {
    if (!directory) return env->NewStringUTF("invalid-media-directory:null");
    auto* graph = guitarrackcraft::nativeRackGraph();
    if (!graph) return env->NewStringUTF("engine-unavailable");
    const char* chars = env->GetStringUTFChars(directory, nullptr);
    if (!chars) return env->NewStringUTF("invalid-media-directory");
    std::string diagnostic;
    graph->materializeProjectMedia(chars, diagnostic);
    env->ReleaseStringUTFChars(directory, chars);
    return diagnostic.empty() ? nullptr : env->NewStringUTF(diagnostic.c_str());
}
