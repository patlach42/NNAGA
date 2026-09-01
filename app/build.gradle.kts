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

import com.android.build.api.artifact.SingleArtifact
import java.math.BigInteger
import org.gradle.api.invocation.Gradle
import org.gradle.api.tasks.Sync

import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Properties



plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
val projectRoot = rootProject.projectDir
val versionPropertiesFile = rootProject.file("version.properties")
val requiredVersionPropertyKeys = setOf("VERSION_NAME", "VERSION_CODE")
val semVerPattern = Regex(
    "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)" +
        "(?:-(?:(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))" +
        "(?:\\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?" +
        "(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$"
)

if (!versionPropertiesFile.isFile) {
    error("Missing application version file: ${versionPropertiesFile.absolutePath}")
}

val versionAssignmentPattern = Regex("^([A-Z_]+)=(.*)$")
val seenVersionPropertyKeys = mutableSetOf<String>()
versionPropertiesFile.readLines().forEachIndexed { index, line ->
    val trimmed = line.trim()
    if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith("!")) {
        return@forEachIndexed
    }
    val key = versionAssignmentPattern.matchEntire(line)?.groupValues?.get(1)
        ?: error("Invalid version.properties assignment at line ${index + 1}")
    require(key in requiredVersionPropertyKeys) {
        "Unknown version.properties key at line ${index + 1}: $key"
    }
    require(seenVersionPropertyKeys.add(key)) {
        "Duplicate version.properties key at line ${index + 1}: $key"
    }
}
require(seenVersionPropertyKeys == requiredVersionPropertyKeys) {
    "version.properties must contain exactly: ${requiredVersionPropertyKeys.sorted().joinToString()}"
}

val versionProperties = Properties().also { properties ->
    versionPropertiesFile.inputStream().use(properties::load)
}
require(versionProperties.stringPropertyNames() == requiredVersionPropertyKeys) {
    "version.properties must contain exactly: ${requiredVersionPropertyKeys.sorted().joinToString()}"
}
val applicationVersionName = versionProperties.getProperty("VERSION_NAME")
    ?.takeIf { it.isNotBlank() }
    ?: error("Missing or blank VERSION_NAME in version.properties")
require(semVerPattern.matches(applicationVersionName)) {
    "VERSION_NAME must be a complete SemVer 2.0.0 version without a leading v"
}
val applicationVersionCodeText = versionProperties.getProperty("VERSION_CODE")
    ?.takeIf { it.isNotBlank() }
    ?: error("Missing or blank VERSION_CODE in version.properties")
require(applicationVersionCodeText.matches(Regex("^[1-9][0-9]*$"))) {
    "VERSION_CODE must be canonical ASCII decimal"
}
val applicationVersionCode = applicationVersionCodeText.toLongOrNull()
    ?: error("VERSION_CODE is outside the supported range")
require(applicationVersionCode in 1..2_100_000_000) {
    "VERSION_CODE must be between 1 and 2100000000"
}

fun gitOutput(vararg arguments: String): String? = runCatching {
    val process = ProcessBuilder(listOf("git") + arguments)
        .directory(projectRoot)
        .redirectErrorStream(true)
        .start()
    val output = process.inputStream.bufferedReader().use { it.readText() }.trim()
    if (process.waitFor() == 0) output else null
}.getOrNull()

fun base26Suffix(index: BigInteger): String {
    require(index.signum() >= 0) { "Build suffix index must be nonnegative" }
    val alphabetSize = BigInteger.valueOf(26L)
    return buildString {
        var remaining = index
        while (remaining.signum() > 0) {
            remaining = remaining.subtract(BigInteger.ONE)
            insert(0, ('a'.code + remaining.mod(alphabetSize).toInt()).toChar())
            remaining = remaining.divide(alphabetSize)
        }
    }
}

val versionLinePattern = "^VERSION_NAME=${applicationVersionName.replace(".", "\\.").replace("+", "\\+")}$"
val baselineCommit = gitOutput(
    "log", "-n", "1", "--format=%H", "-G", versionLinePattern, "--", "version.properties"
)
val gitCommitCount = baselineCommit
    ?.let { gitOutput("rev-list", "--count", "$it..HEAD")?.toBigIntegerOrNull() }
    ?.takeIf { it.signum() >= 0 }
    ?: BigInteger.ZERO
val gitDirty = gitOutput(
    "status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=dirty"
)?.isNotEmpty() == true
val dirtyVersionFile = rootProject.file("dirty.version")
val dirtyBuildIndex = if (gitDirty) {
    val stored = if (dirtyVersionFile.isFile) dirtyVersionFile.readText() else ""
    if (stored.isEmpty()) {
        BigInteger.ONE
    } else {
        require(stored.matches(Regex("^(0|[1-9][0-9]*)$"))) {
            "dirty.version must contain a canonical nonnegative ASCII decimal"
        }
        stored.toBigInteger()
    }
} else {
    BigInteger.ZERO
}
val applicationVersionDisplayName = applicationVersionName +
    base26Suffix(gitCommitCount) +
    if (gitDirty) "-dirty-${base26Suffix(dirtyBuildIndex)}" else ""

fun incrementDirtyVersionCounter(file: java.io.File) {
    FileChannel.open(
        file.toPath(),
        StandardOpenOption.CREATE,
        StandardOpenOption.READ,
        StandardOpenOption.WRITE
    ).use { channel ->
        channel.lock().use {
            val bytes = ByteArray(channel.size().toInt())
            if (bytes.isNotEmpty()) channel.read(ByteBuffer.wrap(bytes), 0)
            val stored = String(bytes, StandardCharsets.UTF_8)
            val current = if (stored.isEmpty()) {
                BigInteger.ONE
            } else {
                require(stored.matches(Regex("^(0|[1-9][0-9]*)$"))) {
                    "dirty.version must contain a canonical nonnegative ASCII decimal"
                }
                stored.toBigInteger()
            }
            val next = current.add(BigInteger.ONE).toString().toByteArray(StandardCharsets.UTF_8)
            channel.truncate(0)
            channel.position(0)
            channel.write(ByteBuffer.wrap(next))
            channel.force(true)
        }
    }
}

val gradleRef: Gradle = gradle
gradleRef.buildFinished {
    val artifactTasks = gradleRef.taskGraph.allTasks.filter {
        it.path.startsWith(":app:assemble") || it.path.startsWith(":app:bundle")
    }
    if (
        gitDirty &&
        System.getenv("GITHUB_ACTIONS") != "true" &&
        failure == null &&
        artifactTasks.any { it.state.executed && it.state.failure == null }
    ) {
        incrementDirtyVersionCounter(dirtyVersionFile)
    }
}

android {
    namespace = "com.vibes.dsp"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.vibes.dsp"
        minSdk = 26
        targetSdk = 35
        versionCode = applicationVersionCode.toInt()
        versionName = applicationVersionName

        val buildDate = SimpleDateFormat("yyyy-MM-dd").format(Date())
        val buildTime = SimpleDateFormat("HH:mm").format(Date())
        val buildHost = System.getenv("HOSTNAME")
            ?: System.getenv("COMPUTERNAME")
            ?: try { InetAddress.getLocalHost().hostName } catch (_: Exception) { "unknown" }
        buildConfigField("String", "BUILD_DATE", "\"$buildDate\"")
        buildConfigField("String", "BUILD_TIME", "\"$buildTime\"")
        buildConfigField("String", "BUILD_HOST", "\"$buildHost\"")
        buildConfigField("String", "VERSION_DISPLAY_NAME", "\"$applicationVersionDisplayName\"")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_shared")
                val audioPgoMode =
                    (project.findProperty("grcAudioPgoMode")?.toString() ?: "OFF").uppercase()
                arguments += "-DGRC_AUDIO_PGO_MODE=$audioPgoMode"
                project.findProperty("grcAudioPgoProfile")?.toString()
                    ?.takeIf { it.isNotBlank() }
                    ?.let { arguments += "-DGRC_AUDIO_PGO_PROFILE=${rootProject.file(it).absolutePath}" }
            }
        }

        ndk {
            // LV2 libs (lilv, etc.) are built for arm64-v8a only; use arm64-v8a until armeabi-v7a libs are built
            abiFilters += listOf("arm64-v8a")
        }
    }

    signingConfigs {
        fun requiredSigningProperty(name: String): String =
            project.findProperty(name)?.toString()?.takeIf { it.isNotBlank() }
                ?: error("Missing required signing property: $name")

        create("release") {
            val ksFile = requiredSigningProperty("RELEASE_STORE_FILE")
            val ksPass = requiredSigningProperty("RELEASE_STORE_PASSWORD")
            val kAlias = requiredSigningProperty("RELEASE_KEY_ALIAS")
            val kPass = requiredSigningProperty("RELEASE_KEY_PASSWORD")
            val keystore = file(ksFile)
            require(keystore.isFile) {
                "Keystore specified by RELEASE_STORE_FILE does not exist: ${keystore.absolutePath}"
            }
            storeFile = keystore
            storePassword = ksPass
            keyAlias = kAlias
            keyPassword = kPass
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("release")
        }
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
    }

    flavorDimensions += "distribution"
    productFlavors {
        create("full") {
            dimension = "distribution"
            // Wine PE relocation needs pre-Android-10 SELinux execmod on
            // app_data_file (denied at targetSdk >= 29). The full flavor
            // bundles :vsthost_lib (~1 GB of wine + FEX) and stays at 28
            // for that reason; sideload-only distribution (F-Droid, direct
            // APK). See plan: prepare-plan-for-integrating-toasty-knuth.md
            // and memory: feedback_targetsdk35_blocked.md.
            targetSdk = 28
            buildConfigField("boolean", "USE_ASSET_PACKS", "false")
            buildConfigField("boolean", "HAS_VST_HOST",   "true")
            externalNativeBuild {
                cmake {
                    // Toggle the VstFactory registration block in
                    // app/src/main/cpp/jni/NativeBridge.cpp.
                    arguments += "-DHAS_VST_HOST=1"
                }
            }
        }
        create("playstore") {
            dimension = "distribution"
            // Play Store eligible (current floor: targetSdk=35 as of
            // Aug 2025). No :vsthost_lib dependency, no wine, "Manage VST"
            // overflow entry hidden via BuildConfig.HAS_VST_HOST=false.
            targetSdk = 35
            buildConfigField("boolean", "USE_ASSET_PACKS", "true")
            buildConfigField("boolean", "HAS_VST_HOST",   "false")
        }
    }

    assetPacks += listOf(":gxplugins_pack", ":neural_pack", ":brummer_pack")

    lint {
        checkReleaseBuilds = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
        // Consume :vsthost_lib's prefab package (full flavor only — playstore
        // never depends on :vsthost_lib so find_package returns NOTFOUND there).
        prefab = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.4"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            useLegacyPackaging = true
            // build.sh renames SONAME-versioned libs (libxcb.so.1 -> libxcb.so) so Android
            // extracts them to nativeLibDir. Only liblilv-0.so.0 keeps its versioned name.
            pickFirsts += listOf("**/liblilv-0.so.0")
        }
    }

    // Copy liblilv-0.so.0 (versioned SONAME) to merged/stripped native libs
    afterEvaluate {
        val jniDir = file("src/main/jniLibs/arm64-v8a")
        val lv2LibDir = file("src/main/cpp/libs/lv2/lib")

        fun copyExtraNativeLibs(targetDir: java.io.File) {
            if (!targetDir.exists()) return
            // liblilv-0.so.0 needs special handling: versioned SONAME file + unversioned alias
            var liblilvSo0 = file("$jniDir/liblilv-0.so.0")
            if (!liblilvSo0.exists()) liblilvSo0 = file("$lv2LibDir/liblilv-0.so.0")
            if (liblilvSo0.exists()) {
                copy { from(liblilvSo0); into(targetDir) }
                copy { from(liblilvSo0); into(targetDir); rename { "liblilv-0.so" } }
                println("Copied liblilv-0.so.0 (+ alias liblilv-0.so) to ${targetDir.absolutePath}")
            }
        }

        for (flavor in listOf("Full", "Playstore")) {
            for (buildType in listOf("Debug", "Release")) {
                val variant = "$flavor$buildType"
                val varLower = variant.replaceFirstChar { it.lowercase() }
                tasks.findByName("merge${variant}NativeLibs")?.doLast {
                    copyExtraNativeLibs(file("build/intermediates/merged_native_libs/$varLower/out/lib/arm64-v8a"))
                }
                tasks.findByName("strip${variant}DebugSymbols")?.doLast {
                    copyExtraNativeLibs(file("build/intermediates/stripped_native_libs/$varLower/out/lib/arm64-v8a"))
                }
            }
        }
    }

    // Debug APKs are intentionally small: retain only NAM and AIDA LV2
    // bundles/libraries. Release variants continue to receive the complete
    // payload staged by build.sh.
    val debugLv2Bundles = setOf("aidadsp.lv2", "AIDA-X.lv2", "neural_amp_modeler.lv2")
    val debugNativeLibraries = setOf("libneural_amp_modeler.so", "librt-neural-generic.so")

    fun isDebugPluginLibrary(name: String): Boolean {
        if (name in debugNativeLibraries || name.startsWith("libAIDA-X")) return false
        return name.startsWith("libgx") ||
            name.startsWith("libGx") ||
            name.startsWith("libNeural") ||
            name.startsWith("libneural") ||
            name.startsWith("libAIDA") ||
            name.startsWith("librt-neural") ||
            name.startsWith("libCollisionDrive") ||
            name.startsWith("libFatFrog") ||
            name.startsWith("libMetalTone") ||
            name.startsWith("libXDarkTerror") ||
            name.startsWith("libXTinyTerror") ||
            name.startsWith("libImpulseLoader") ||
            name.startsWith("libPowerAmp") ||
            name.startsWith("libpoweramps") ||
            name.startsWith("libPreAmp") ||
            name.startsWith("libGxCabSim") ||
            name.startsWith("libdoubletracker")
    }

    fun filterDebugAssets(taskName: String, variant: String) {
        tasks.matching { it.name == taskName }.configureEach {
            val task = this
            task.outputs.upToDateWhen { false }
            task.doLast {
                val mergedAssets = file("build/intermediates/assets/$variant/merge${variant.replaceFirstChar { it.uppercase() }}Assets")
                val lv2Dir = file("$mergedAssets/lv2")
                if (!lv2Dir.isDirectory) return@doLast
                lv2Dir.listFiles()?.filter { it.isDirectory && it.name !in debugLv2Bundles }
                    ?.forEach { it.deleteRecursively() }
                mergedAssets.resolve("lv2_bundles.txt").writeText(
                    debugLv2Bundles.filter { file("$lv2Dir/$it").isDirectory }.sorted().joinToString("\n") + "\n"
                )
                val files = lv2Dir.walkTopDown().filter { it.isFile }
                    .map { it.relativeTo(lv2Dir).invariantSeparatorsPath }
                    .sorted().toList()
                mergedAssets.resolve("lv2_files.txt").writeText(files.joinToString("\n") + "\n")
            }
        }
    }

    fun filterDebugJni(taskName: String, variant: String) {
        tasks.matching { it.name == taskName }.configureEach {
            val task = this
            task.outputs.upToDateWhen { false }
            task.doLast {
                val mergedJni = file("build/intermediates/merged_jni_libs/$variant/merge${variant.replaceFirstChar { it.uppercase() }}JniLibFolders/out")
                if (!mergedJni.isDirectory) return@doLast
                mergedJni.walkTopDown().filter { it.isFile && isDebugPluginLibrary(it.name) }
                    .forEach { it.delete() }
            }
        }
    }

    fun filterBasePluginPayload(taskName: String, variant: String) {
        tasks.matching { it.name == taskName }.configureEach {
            outputs.upToDateWhen { false }
            doLast {
                val mergedAssets = file("build/intermediates/assets/$variant/merge${variant.replaceFirstChar { it.uppercase() }}Assets")
                mergedAssets.resolve("lv2").deleteRecursively()
                mergedAssets.resolve("plugin_libs.txt").delete()
                mergedAssets.resolve("lv2_bundles.txt").delete()
                mergedAssets.resolve("lv2_files.txt").delete()
                val mergedJni = file("build/intermediates/merged_jni_libs/$variant/merge${variant.replaceFirstChar { it.uppercase() }}JniLibFolders/out")
                val pluginPrefixes = listOf("libgx", "libGx", "libAIDA", "libNeural", "libneural", "librt-neural", "libCollisionDrive", "libFatFrog", "libMetalTone", "libXDarkTerror", "libXTinyTerror", "libImpulseLoader", "libPowerAmp", "libpoweramps", "libPreAmp", "libGxCabSim", "libdoubletracker")
                mergedJni.walkTopDown().filter { it.isFile && pluginPrefixes.any(it.name::startsWith) }.forEach { it.delete() }
            }
        }
    }

    for (variant in listOf("fullDebug", "fullRelease", "playstoreDebug", "playstoreRelease")) {
        filterBasePluginPayload("merge${variant.replaceFirstChar { it.uppercase() }}Assets", variant)
        filterBasePluginPayload("merge${variant.replaceFirstChar { it.uppercase() }}JniLibFolders", variant)
    }

    for (variant in listOf("fullDebug", "playstoreDebug")) {
        filterDebugAssets("merge${variant.replaceFirstChar { it.uppercase() }}Assets", variant)
        filterDebugJni("merge${variant.replaceFirstChar { it.uppercase() }}JniLibFolders", variant)
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs("src/main/assets")
        }
    }
}

fun versionedArtifactName(sourceName: String, extension: String): String {
    val suffix = sourceName.removePrefix("app-")
    require(
        suffix != sourceName &&
            suffix.length > extension.length + 1 &&
            suffix.endsWith(".$extension")
    ) {
        "Unexpected Android $extension artifact basename: $sourceName"
    }
    return "nnaga-$applicationVersionName-$suffix"
}

androidComponents {
    onVariants(selector().all()) { variant ->
        val taskSuffix = variant.name.replaceFirstChar { it.uppercase() }
        val assembleTaskName = "assemble$taskSuffix"
        val bundleTaskName = "bundle$taskSuffix"
        val packageVersionedApk = tasks.register<Sync>("package${taskSuffix}VersionedApk") {
            group = "build"
            description = "Copies $taskSuffix APKs to the canonical versioned output directory."
            from(variant.artifacts.get(SingleArtifact.APK)) {
                include("*.apk")
                rename { sourceName -> versionedArtifactName(sourceName, "apk") }
            }
            into(layout.buildDirectory.dir("outputs/versioned/apk/${variant.name}"))
            doFirst {
                val sourceApks = source.files.filter { it.isFile && it.extension == "apk" }
                require(sourceApks.isNotEmpty()) {
                    "No APK artifacts were produced for ${variant.name}"
                }
                sourceApks.forEach { versionedArtifactName(it.name, "apk") }
            }
            onlyIf("the APK producer completed successfully") {
                project.tasks.findByName(assembleTaskName)?.state?.let {
                    it.executed && it.failure == null
                } == true
            }
        }
        val packageVersionedBundle = tasks.register<Sync>("package${taskSuffix}VersionedBundle") {
            group = "build"
            description = "Copies the $taskSuffix bundle to the canonical versioned output directory."
            from(variant.artifacts.get(SingleArtifact.BUNDLE)) {
                include("*.aab")
                rename { sourceName -> versionedArtifactName(sourceName, "aab") }
            }
            into(layout.buildDirectory.dir("outputs/versioned/bundle/${variant.name}"))
            doFirst {
                val sourceBundles = source.files.filter { it.isFile && it.extension == "aab" }
                require(sourceBundles.size == 1) {
                    "Expected exactly one AAB artifact for ${variant.name}, found ${sourceBundles.size}"
                }
                versionedArtifactName(sourceBundles.single().name, "aab")
            }
            onlyIf("the bundle producer completed successfully") {
                project.tasks.findByName(bundleTaskName)?.state?.let {
                    it.executed && it.failure == null
                } == true
            }
        }
        tasks.matching { it.name == assembleTaskName }.configureEach {
            finalizedBy(packageVersionedApk)
        }
        tasks.matching { it.name == bundleTaskName }.configureEach {
            finalizedBy(packageVersionedBundle)
        }
    }
}

// Full variants must never package without the staged Wine/FEX runtime payload.
val verifyFullRuntimePayload = tasks.register("verifyFullRuntimePayload") {
    doLast {
        val jniLibDir = rootProject.file("vsthost_lib/src/main/jniLibs/arm64-v8a")
        val assetsDir = rootProject.file("vsthost_lib/src/main/assets")
        val missing = mutableListOf<String>()

        if (jniLibDir.listFiles()?.none {
                it.isFile && it.name.matches(Regex("libwine_.*\\.so"))
            } != false) {
            missing += "${jniLibDir.relativeTo(rootProject.projectDir).path}/libwine_*.so"
        }
        if (!assetsDir.resolve("wine-fex-manifest.json").isFile) {
            missing += "${assetsDir.relativeTo(rootProject.projectDir).path}/wine-fex-manifest.json"
        }
        for (hostAsset in listOf("vst_host.exe", "vst_host_x86.exe", "vst3_host.exe")) {
            if (!assetsDir.resolve(hostAsset).isFile) {
                missing += "${assetsDir.relativeTo(rootProject.projectDir).path}/$hostAsset"
            }
        }
        if (!assetsDir.resolve("wine-fex-nls.tar").isFile &&
            !assetsDir.resolve("wine-fex-nls.tar.gz").isFile
        ) {
            missing += "${assetsDir.relativeTo(rootProject.projectDir).path}/wine-fex-nls.tar[.gz]"
        }
        for (rendererArchive in listOf("turnip-libs", "mesa-zink-libs")) {
            if (!assetsDir.resolve("$rendererArchive.tar").isFile &&
                !assetsDir.resolve("$rendererArchive.tar.gz").isFile
            ) {
                missing += "${assetsDir.relativeTo(rootProject.projectDir).path}/$rendererArchive.tar[.gz]"
            }
        }

        if (missing.isNotEmpty()) {
            throw GradleException(
                "Full Wine/FEX runtime payload verification failed. " +
                    "Missing required staged files/categories:\n" +
                    missing.joinToString(separator = "\n") { " - $it" }
            )
        }
        logger.lifecycle("Full Wine/FEX runtime payload verified.")
    }
}

// Wire lazily because Android Gradle Plugin creates variant tasks after this script
// is evaluated. Playstore tasks are intentionally untouched.
tasks.configureEach {
    if (name in setOf("assembleFullDebug", "assembleFullRelease", "bundleFullDebug", "bundleFullRelease")) {
        dependsOn(verifyFullRuntimePayload)
    }
}

dependencies {
    // X11 plugin UIs: native EGL + ANativeWindow (see app/src/main/cpp/x11/)

    // VST hosting (wine + FEX, ~1 GB) — only in the `full` flavor, never in
    // `playstore`. See plan: prepare-plan-for-integrating-toasty-knuth.md.
    "fullApi"(project(":vsthost_lib"))

    // Core Android
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
    implementation("androidx.activity:activity-compose:1.8.1")

    // Compose
    implementation(platform("androidx.compose:compose-bom:2023.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    // ViewModel
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.6.2")

    // Navigation
    implementation("androidx.navigation:navigation-compose:2.7.5")

    // WebView safe file access (avoid file:// access denied on API 29+)
    implementation("androidx.webkit:webkit:1.8.0")

    // Permissions
    implementation("com.google.accompanist:accompanist-permissions:0.32.0")

    // Networking & JSON
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.google.code.gson:gson:2.10.1")
    implementation("org.tomlj:tomlj:1.1.1")

    // Image loading
    implementation("io.coil-kt:coil-compose:2.5.0")

    // Testing
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test:rules:1.5.0")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
    androidTestImplementation("androidx.test.uiautomator:uiautomator:2.3.0")
    androidTestImplementation(platform("androidx.compose:compose-bom:2023.10.01"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
