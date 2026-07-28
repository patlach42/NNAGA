package com.varcain.guitarrackcraft.engine

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
    fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = native.loadTrackWav(trackId, path, displayName)
    fun unloadTrackWav(trackId: RackPathId): Boolean = native.unloadTrackWav(trackId)
    fun clearTrackWavs(): Boolean = native.clearTrackWavs()
    fun setWavTransportPlaying(playing: Boolean): Boolean = native.setWavTransportPlaying(playing)
    fun restartWavTransport(): Boolean = native.restartWavTransport()
    fun setWavTransportLooping(looping: Boolean) = native.setWavTransportLooping(looping)
    fun getWavTransportInfo(): WavTransportInfo = native.getWavTransportInfo()

    fun saveRackState(): String? = native.saveRackState()
    fun restorePluginState(pathId: RackPathId, pluginIndex: Int, portValues: FloatArray, portIndices: IntArray, propertyKeys: Array<String>, propertyTypes: Array<String>, propertyValues: Array<ByteArray>, propertyFlags: IntArray): Boolean =
        native.restorePluginState(pathId, pluginIndex, portValues, portIndices, propertyKeys, propertyTypes, propertyValues, propertyFlags)
}
