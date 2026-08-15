package com.vibes.dsp

/**
 * Optional full-flavor task started with the application process. Core engine
 * initialization must not await it; callers may await it after the engine is
 * ready to refresh optional plugin facilities.
 */
interface StartupPrerequisite {
    suspend fun awaitStartupPrerequisite(): Boolean
}
