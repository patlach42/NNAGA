package com.vibes.dsp.engine

import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow

data class ModelLoadedEvent(val pathId: RackPathId, val pluginIndex: Int, val modelName: String)
data class VstTone3000FileSelectedEvent(val pathId: RackPathId, val pluginIndex: Int, val filePath: String)

/** Path-aware facade for the parallel track/master rack graph. */
object RackManager {
    private val native get() = NativeEngine.getInstance()

    private val _modelLoadedEvents = MutableSharedFlow<ModelLoadedEvent>(extraBufferCapacity = 1)
    val modelLoadedEvents = _modelLoadedEvents.asSharedFlow()
    private val _vstTone3000FileSelectedEvents = MutableSharedFlow<VstTone3000FileSelectedEvent>(extraBufferCapacity = 1)
    val vstTone3000FileSelectedEvents = _vstTone3000FileSelectedEvents.asSharedFlow()

    fun notifyModelLoaded(pathId: RackPathId, pluginIndex: Int, modelName: String) =
        _modelLoadedEvents.tryEmit(ModelLoadedEvent(pathId, pluginIndex, modelName))
    fun notifyVstTone3000FileSelected(pathId: RackPathId, pluginIndex: Int, filePath: String) =
        _vstTone3000FileSelectedEvents.tryEmit(VstTone3000FileSelectedEvent(pathId, pluginIndex, filePath))

    fun getAvailablePlugins(): List<PluginInfo> = native.getAvailablePlugins()
    fun addPlugin(pathId: RackPathId, pluginId: String, position: Int = -1): Int = native.addPluginToRack(pathId, pluginId, position)
    fun removePlugin(pathId: RackPathId, position: Int): Boolean = native.removePluginFromRack(pathId, position)
    fun reorder(pathId: RackPathId, fromPos: Int, toPos: Int): Boolean = native.reorderRack(pathId, fromPos, toPos)
    fun setPluginFilePath(pathId: RackPathId, pluginIndex: Int, propertyUri: String, filePath: String) = native.setPluginFilePath(pathId, pluginIndex, propertyUri, filePath)
    fun setParameter(pathId: RackPathId, pluginIndex: Int, portIndex: Int, value: Float) = native.setParameter(pathId, pluginIndex, portIndex, value)
    fun getParameter(pathId: RackPathId, pluginIndex: Int, portIndex: Int): Float = native.getParameter(pathId, pluginIndex, portIndex)
    fun getRackSize(pathId: RackPathId): Int = native.getRackSize(pathId)
    fun getRackPluginInfo(pathId: RackPathId, index: Int): PluginInfo? = native.getRackPluginInfo(pathId, index)
    fun getRackPluginInstanceId(pathId: RackPathId, index: Int): Long = native.getRackPluginInstanceId(pathId, index)
    fun getRackPlugins(pathId: RackPathId): Array<RackPluginEntry> = native.getRackPlugins(pathId)

    fun addTrack(): RackPathId = native.addTrack()
    fun removeTrack(trackId: RackPathId): Boolean = native.removeTrack(trackId)
    fun getTracks(): Array<RackTrackInfo> = native.getTracks()
    fun setTrackVolume(trackId: RackPathId, volume: Float): Boolean = native.setTrackVolume(trackId, volume)
    fun setTrackInputArmed(trackId: RackPathId, armed: Boolean): Boolean = native.setTrackInputArmed(trackId, armed)
    fun setTrackInputChannel(trackId: RackPathId, inputChannel: Int): Boolean =
        native.setTrackInputChannel(trackId, inputChannel)
    fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = native.loadTrackWav(trackId, path, displayName)
    fun loadTrackMidi(trackId: RackPathId, path: String, displayName: String): Boolean =
        native.loadTrackMidi(trackId, path, displayName)
    fun unloadTrackMidi(trackId: RackPathId): Boolean = native.unloadTrackMidi(trackId)
    fun unloadTrackWav(trackId: RackPathId): Boolean = native.unloadTrackWav(trackId)
    fun clearTrackWavs(): Boolean = native.clearTrackWavs()
    fun getTrackWaveformPeaks(trackId: RackPathId, maxBuckets: Int = 256): FloatArray =
        native.nativeGetTrackWaveformPeaks(trackId, maxBuckets)
    fun getTrackClipSlots(trackId: RackPathId): Array<ClipSlotInfo> = native.nativeGetTrackClipSlots(trackId)
    fun getTrackClipMidiNotes(trackId: RackPathId, slot: Int): Array<MidiNoteInfo> =
        native.nativeGetTrackClipMidiNotes(trackId, slot)
    fun loadTrackClipWav(trackId: RackPathId, slot: Int, path: String, displayName: String): Boolean =
        native.nativeLoadTrackClipWav(trackId, slot, path, displayName)
    fun loadTrackClipMidi(trackId: RackPathId, slot: Int, path: String, displayName: String): Boolean =
        native.nativeLoadTrackClipMidi(trackId, slot, path, displayName)
    fun selectTrackClipSlot(trackId: RackPathId, slot: Int): Boolean =
        native.nativeSelectTrackClipSlot(trackId, slot)
    fun setTrackTransportPlaying(
        trackId: RackPathId,
        playing: Boolean,
        quantization: TrackLaunchQuantization
    ): Boolean = native.setTrackTransportPlaying(trackId, playing, quantization)
    fun setTrackTransportLooping(trackId: RackPathId, looping: Boolean): Boolean =
        native.setTrackTransportLooping(trackId, looping)
    fun startTrackLoopRecording(
        trackId: RackPathId,
        bars: Double,
        quantization: TrackLaunchQuantization,
        enterOnPunch: Boolean
    ): Boolean = native.startTrackLoopRecording(trackId, bars, quantization, enterOnPunch)
    fun startTrackClipRecording(
        trackId: RackPathId,
        slot: Int,
        bars: Double,
        quantization: TrackLaunchQuantization,
        enterOnPunch: Boolean
    ): Boolean = native.startTrackClipRecording(trackId, slot, bars, quantization, enterOnPunch)
    fun cancelTrackLoopRecording(trackId: RackPathId): Boolean =
        native.cancelTrackLoopRecording(trackId)
    fun setTransportBpm(bpm: Double): Boolean = native.setTransportBpm(bpm)
    fun setTransportPlaying(playing: Boolean): Boolean = native.setTransportPlaying(playing)
    fun restartTransport(): Boolean = native.restartTransport()
    fun getTransportInfo(): TransportInfo = native.getTransportInfo()

}
