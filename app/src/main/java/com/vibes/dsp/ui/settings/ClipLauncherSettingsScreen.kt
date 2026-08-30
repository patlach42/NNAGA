/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.settings

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaSwitch
import com.vibes.dsp.ui.components.NnagaTextButton
import com.vibes.dsp.ui.live.ClipLauncherPreferences

@Composable
fun ClipLauncherSettingsScreen() {
    val context = LocalContext.current
    var browserRootUri by remember { mutableStateOf(ClipLauncherPreferences.getBrowserRootUri(context)) }
    var browserRootDisplayName by remember(browserRootUri) {
        mutableStateOf(browserRootUri?.let { resolveBrowserRootDisplayName(context, it) })
    }
    var browserRootAccessError by remember {
        mutableStateOf<String?>(null)
    }
    var copyClipsIntoProject by remember {
        mutableStateOf(ClipLauncherPreferences.getCopyClipsIntoProject(context))
    }
    var autoDetectBpm by remember {
        mutableStateOf(ClipLauncherPreferences.getAutoDetectBpmFromFilename(context))
    }
    var autoDetectLoopTempo by remember {
        mutableStateOf(ClipLauncherPreferences.getAutoDetectLoopTempo(context))
    }

    val browserFolderPickerLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult

        val chosenDisplayName = runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
            resolveBrowserRootDisplayName(context, uri)
                ?: throw IllegalStateException("Folder display name unavailable")
        }.getOrNull()

        if (chosenDisplayName == null) {
            browserRootAccessError = "Folder access could not be saved"
            return@rememberLauncherForActivityResult
        }

        ClipLauncherPreferences.setBrowserRootUri(context, uri)
        browserRootUri = uri
        browserRootDisplayName = chosenDisplayName
        browserRootAccessError = null
    }


    val browserRootUriText = browserRootUri?.toString() ?: ""

    val isBrowserRootUnavailable = browserRootUri != null && browserRootDisplayName == null

    Column(
        modifier = Modifier
            .padding(
                horizontal = SettingsDimensions.contentPadding,
                vertical = SettingsDimensions.spacing
            )
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(SettingsDimensions.spacing)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.Top,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Clip browser folder",
                    style = MaterialTheme.typography.bodyMedium
                )
                when {
                    browserRootUri == null -> {
                        Text(
                            text = "No folder selected",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    isBrowserRootUnavailable -> {
                        Text(
                            text = "Folder access unavailable",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.error
                        )
                    }
                    else -> {
                        Text(
                            text = browserRootDisplayName ?: browserRootUriText,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Text(
                            text = browserRootUriText,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                browserRootAccessError?.let { error ->
                    Text(
                        text = error,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.error
                    )
                }
            }
            Row(
                horizontalArrangement = Arrangement.spacedBy(SettingsDimensions.spacing)
            ) {
                NnagaButton(onClick = { browserFolderPickerLauncher.launch(null) }) { Text("Choose folder") }
                if (browserRootUri != null) {
                    NnagaTextButton(
                        onClick = {
                            ClipLauncherPreferences.setBrowserRootUri(context, null)
                            browserRootUri = null
                            browserRootDisplayName = null
                            browserRootAccessError = null
                        }
                    ) { Text("Clear") }
                }
            }
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Detect BPM from WAV filename",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "Recognized BPM becomes the clip's Base BPM and enables Stretch mode.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            NnagaSwitch(
                checked = autoDetectBpm,
                onCheckedChange = { enabled ->
                    autoDetectBpm = enabled
                    ClipLauncherPreferences.setAutoDetectBpmFromFilename(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Detect BPM from WAV filename"
                }
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Autodetect loop tempo",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "Estimate loop tempo from 2, 4, 8, or 16 bars within 50–200 BPM.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            NnagaSwitch(
                checked = autoDetectLoopTempo,
                onCheckedChange = { enabled ->
                    autoDetectLoopTempo = enabled
                    ClipLauncherPreferences.setAutoDetectLoopTempo(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Autodetect loop tempo"
                }
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Copy clips into project",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "When saving, copy clip files into project folder instead of only saving source links.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            NnagaSwitch(
                checked = copyClipsIntoProject,
                onCheckedChange = { enabled ->
                    copyClipsIntoProject = enabled
                    ClipLauncherPreferences.setCopyClipsIntoProject(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Copy clips into project"
                }
            )
        }
    }
}

private fun resolveBrowserRootDisplayName(context: Context, treeUri: Uri): String? = runCatching {
    val documentId = DocumentsContract.getTreeDocumentId(treeUri)
    val documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId)
    context.contentResolver.query(
        documentUri,
        arrayOf(OpenableColumns.DISPLAY_NAME),
        null,
        null,
        null
    )?.use { cursor ->
        if (!cursor.moveToFirst()) return@runCatching null
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (nameIndex < 0) return@runCatching null
        cursor.getString(nameIndex)
    }
}.getOrNull()
