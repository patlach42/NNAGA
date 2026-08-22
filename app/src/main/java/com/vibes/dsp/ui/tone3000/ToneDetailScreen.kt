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

package com.vibes.dsp.ui.tone3000

import android.content.Intent
import android.net.Uri
import java.net.URLEncoder
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaTonalButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.CloudDownload
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Person
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.request.ImageRequest

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ToneDetailScreen(
    toneId: String,
    initialTone: Tone? = null,
    onNavigateBack: () -> Unit,
    sourcePathId: Long = -1L,
    sourcePluginIndex: Int = -1,
    sourceSlot: String? = null,
    architecture: String? = null,
    viewModel: ToneDetailViewModel = viewModel()
) {
    val tone by viewModel.tone.collectAsState()
    val models by viewModel.models.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val error by viewModel.error.collectAsState()
    val modelsError by viewModel.modelsError.collectAsState()
    val downloadedIds by viewModel.downloadedModelIds.collectAsState()
    val hasSourcePlugin = sourcePluginIndex >= 0
    val context = LocalContext.current
    val filesDir = context.filesDir
    val snackbarHostState = remember { SnackbarHostState() }

    LaunchedEffect(sourcePathId, sourcePluginIndex, sourceSlot) {
        viewModel.setSourcePlugin(sourcePathId, sourcePluginIndex, sourceSlot)
    }

    LaunchedEffect(toneId, initialTone, architecture) {
        if (initialTone != null) {
            viewModel.setTone(initialTone)
        }
        viewModel.loadToneDetail(toneId, architecture)
    }

    LaunchedEffect(Unit) {
        viewModel.downloadStatus.collect { status ->
            snackbarHostState.showSnackbar(status)
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) }
    ) { padding ->
        if (isLoading && tone == null) {
            Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) {
                CircularProgressIndicator(strokeWidth = 2.dp)
            }
        } else if (error != null && tone == null) {
            Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Text(
                        text = error!!,
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodyMedium
                    )
                    NnagaTonalButton(onClick = { viewModel.loadToneDetail(toneId, architecture) }) {
                        Text("Retry")
                    }
                }
            }
        } else {
            tone?.let { currentTone ->
                LazyColumn(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(padding)
                ) {
                    // Hero image and back button
                    item {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(260.dp)
                        ) {
                            // Image
                            val imageUrl = currentTone.images?.firstOrNull()
                                ?: currentTone.user?.avatar_url
                            if (imageUrl != null) {
                                AsyncImage(
                                    model = ImageRequest.Builder(context)
                                        .data(imageUrl)
                                        .size(800, 520)
                                        .build(),
                                    contentDescription = null,
                                    modifier = Modifier.fillMaxSize(),
                                    contentScale = ContentScale.Crop
                                )
                            } else {
                                Box(
                                    modifier = Modifier
                                        .fillMaxSize()
                                        .background(MaterialTheme.colorScheme.surfaceVariant)
                                )
                            }


                            // Back button
                            NnagaIconButton(
                                onClick = onNavigateBack,
                                modifier = Modifier
                                    .align(Alignment.TopStart)
                                    .padding(8.dp)
                            ) {
                                Icon(
                                    Icons.Default.ArrowBack,
                                    contentDescription = "Back",
                                    tint = Color.White
                                )
                            }

                            // Title overlaid at bottom
                            Column(
                                modifier = Modifier
                                    .align(Alignment.BottomStart)
                                    .padding(horizontal = 16.dp, vertical = 8.dp)
                            ) {
                                Text(
                                    text = currentTone.title,
                                    style = MaterialTheme.typography.headlineSmall,
                                    fontWeight = FontWeight.Bold,
                                    color = MaterialTheme.colorScheme.onSurface
                                )
                            }
                        }
                    }

                    // Author and tags
                    item {
                        Column(
                            modifier = Modifier.padding(horizontal = 12.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            // Author row
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                val avatarUrl = currentTone.user?.avatar_url
                                if (avatarUrl != null) {
                                    AsyncImage(
                                        model = ImageRequest.Builder(context)
                                            .data(avatarUrl)
                                            .size(64, 64)
                                            .build(),
                                        contentDescription = null,
                                        modifier = Modifier
                                            .size(24.dp)
                                            .clip(CircleShape),
                                        contentScale = ContentScale.Crop
                                    )
                                } else {
                                    Surface(
                                        shape = CircleShape,
                                        color = MaterialTheme.colorScheme.primaryContainer,
                                        modifier = Modifier.size(24.dp)
                                    ) {
                                        Box(contentAlignment = Alignment.Center) {
                                            Icon(
                                                Icons.Default.Person,
                                                contentDescription = null,
                                                modifier = Modifier.size(14.dp),
                                                tint = MaterialTheme.colorScheme.onPrimaryContainer
                                            )
                                        }
                                    }
                                }
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(
                                    text = currentTone.user?.username ?: "Unknown",
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.Medium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }

                            // Tags
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                currentTone.formatValue()?.let { format ->
                                    ToneBadge(
                                        text = Platform.fromString(format),
                                        containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                                        contentColor = MaterialTheme.colorScheme.onTertiaryContainer
                                    )
                                }
                                currentTone.gear?.let { gear ->
                                    ToneBadge(
                                        text = Gear.fromString(gear),
                                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                                    )
                                }
                                currentTone.sizesLabel()?.let { size ->
                                    ToneBadge(
                                        text = size,
                                        containerColor = MaterialTheme.colorScheme.surfaceVariant,
                                        contentColor = MaterialTheme.colorScheme.onSurface
                                    )
                                }
                            }
                        }
                    }

                    // Stats row
                    item {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 16.dp),
                            horizontalArrangement = Arrangement.SpaceEvenly
                        ) {
                            StatItem(
                                icon = Icons.Default.Layers,
                                value = "${currentTone.modelCountFor(Architecture.fromValue(architecture))}",
                                label = "Models"
                            )
                            StatItem(
                                icon = Icons.Default.CloudDownload,
                                value = "${currentTone.downloads_count}",
                                label = "Downloads"
                            )
                            StatItem(
                                icon = Icons.Default.Favorite,
                                value = "${currentTone.favorites_count}",
                                label = "Favorites"
                            )
                        }
                    }

                    // Divider
                    item {
                        Divider(
                            modifier = Modifier.padding(horizontal = 12.dp),
                            color = MaterialTheme.colorScheme.outlineVariant
                        )
                    }

                    // Description
                    if (!currentTone.description.isNullOrBlank()) {
                        item {
                            Column(modifier = Modifier.padding(16.dp)) {
                                Text(
                                    text = "About",
                                    style = MaterialTheme.typography.titleMedium,
                                    fontWeight = FontWeight.Bold
                                )
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = currentTone.description,
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    lineHeight = MaterialTheme.typography.bodyMedium.lineHeight
                                )
                            }
                        }
                    }

                    // Models header
                    item {
                        Text(
                            text = "Available Models",
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 12.dp)
                        )
                    }

                    // Models list or error
                    if (modelsError != null) {
                        item {
                            Card(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 12.dp),
                                colors = CardDefaults.cardColors(
                                    containerColor = MaterialTheme.colorScheme.surfaceVariant
                                ),
                                shape = MaterialTheme.shapes.small,
                                elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
                                border = CardDefaults.outlinedCardBorder()
                            ) {
                                Column(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(16.dp),
                                    horizontalAlignment = Alignment.CenterHorizontally,
                                    verticalArrangement = Arrangement.spacedBy(8.dp)
                                ) {
                                    Text(
                                        text = modelsError!!,
                                        color = MaterialTheme.colorScheme.error,
                                        style = MaterialTheme.typography.bodyMedium
                                    )
                                    if (modelsError!!.contains("login", ignoreCase = true)) {
                                        NnagaButton(
                                            onClick = {
                                                val redirectUrl = URLEncoder.encode("guitarrackcraft://tone3000auth", "UTF-8")
                                                val url = "https://www.tone3000.com/api/v1/auth?redirect_url=$redirectUrl"
                                                val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
                                                context.startActivity(intent)
                                            },
                                        ) {
                                            Icon(Icons.Default.Person, contentDescription = null, modifier = Modifier.size(16.dp))
                                            Spacer(modifier = Modifier.width(8.dp))
                                            Text("Login to TONE3000")
                                        }
                                    }
                                }
                            }
                        }
                    } else if (models.isEmpty() && isLoading) {
                        item {
                            Box(
                                modifier = Modifier.fillMaxWidth().padding(12.dp),
                                contentAlignment = Alignment.Center
                            ) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(32.dp),
                                    strokeWidth = 2.dp
                                )
                            }
                        }
                    } else {
                        items(models) { model ->
                            val isDownloaded = downloadedIds.contains(model.id) ||
                                    ToneFileUtils.isModelDownloaded(filesDir, currentTone, model)
                            ModelItem(
                                model = model,
                                isDownloaded = isDownloaded,
                                hasSourcePlugin = hasSourcePlugin,
                                onDownload = {
                                    if (isDownloaded && hasSourcePlugin) {
                                        viewModel.loadModelToPlugin(currentTone, model)
                                    } else if (!isDownloaded) {
                                        viewModel.downloadModel(currentTone, model)
                                    }
                                }
                            )
                        }
                    }

                    item {
                        Spacer(modifier = Modifier.height(32.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun StatItem(icon: ImageVector, value: String, label: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Icon(
            icon,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.height(4.dp))
        Text(
            text = value,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold
        )
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
fun ModelItem(
    model: Model,
    isDownloaded: Boolean = false,
    hasSourcePlugin: Boolean = false,
    onDownload: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        ),
        shape = MaterialTheme.shapes.small,
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        border = CardDefaults.outlinedCardBorder()
    ) {
        Row(
            modifier = Modifier
                .padding(12.dp)
                .fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = model.name,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(modifier = Modifier.height(4.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ToneBadge(
                        text = ModelSize.fromString(model.size),
                        containerColor = MaterialTheme.colorScheme.secondaryContainer,
                        contentColor = MaterialTheme.colorScheme.onSecondaryContainer
                    )
                    model.formatValue()?.let {
                        ToneBadge(
                            text = Platform.fromString(it),
                            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                            contentColor = MaterialTheme.colorScheme.onTertiaryContainer
                        )
                    }
                    model.architecture_version?.let {
                        ToneBadge(
                            text = Architecture.fromString(it),
                            containerColor = MaterialTheme.colorScheme.primaryContainer,
                            contentColor = MaterialTheme.colorScheme.onPrimaryContainer
                        )
                    }
                }
            }
            when {
                isDownloaded && hasSourcePlugin -> {
                    FilledIconButton(
                        onClick = onDownload,
                        modifier = Modifier.size(48.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = Color(0xFF4CAF50)
                        ),
                        shape = MaterialTheme.shapes.small
                    ) {
                        Icon(
                            Icons.Default.PlayArrow,
                            contentDescription = "Load",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }
                isDownloaded -> {
                    FilledIconButton(
                        onClick = {},
                        enabled = false,
                        modifier = Modifier.size(48.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant,
                            disabledContainerColor = MaterialTheme.colorScheme.surfaceVariant
                        ),
                        shape = MaterialTheme.shapes.small
                    ) {
                        Icon(
                            Icons.Default.Check,
                            contentDescription = "Downloaded",
                            modifier = Modifier.size(20.dp),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                else -> {
                    FilledIconButton(
                        onClick = onDownload,
                        modifier = Modifier.size(48.dp),
                        colors = IconButtonDefaults.filledIconButtonColors(
                            containerColor = MaterialTheme.colorScheme.primary
                        ),
                        shape = MaterialTheme.shapes.small
                    ) {
                        Icon(
                            Icons.Default.Download,
                            contentDescription = "Download",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }
            }
        }
    }
}
