# NNAGA UI Specification

## Intent

The application uses a **True Black AMOLED** visual system: `#000000` is the screen canvas; content is dense, readable during a performance, and inexpensive to render. The UI must remain usable one-handed without oversized controls.

## Color tokens

All Compose UI obtains colors from `MaterialTheme.colorScheme`; do not introduce per-screen palettes.

| Role | Value | Use |
|---|---:|---|
| `background` | `#000000` | Full-screen canvas and system bars |
| `surface` | `#0A0A0A` | App bars, sheets, dialogs, plugin headers |
| `surfaceVariant` | `#121212` | Secondary sections and control wells |
| `outlineVariant` | `#44473F` | Dividers and flat surface borders |
| `primary` | `#B6F43B` | Primary action, active control, selected state |
| `onBackground` / `onSurface` | `#F5F5F5` | Primary text and icons |
| `onSurfaceVariant` | `#C5C8BD` | Secondary text and metadata |
| `error` | `#FFB4AB` | Errors and clipping/failure status |

Meter thresholds may retain semantic solid green/yellow/red. Any new status color must meet its text/icon contrast requirement on its actual background.

## Layout and interaction

- Use an **8 dp spacing grid**. Default screen/list horizontal padding is **12 dp**; adjacent controls normally use 8 dp.
- Keep controls compact. Every tappable target must measure at least **44 × 44 dp**, including icon-only actions.
- Prefer `Row`/`Column`, `Box`, `LazyColumn`, `Surface`, `OutlinedButton`, `FilterChip`, `Slider`, and standard Material3 controls already available in the app.
- Align label/value/action rows vertically and use `weight()` only for the flexible content region. Do not make a destructive action compete with the main action.
- Group editor controls with flat near-black surfaces and a one-pixel-equivalent low-contrast outline, not floating cards.
- Use `bodySmall`/`labelLarge` for telemetry and metadata; avoid large decorative type in operational screens.

## Surface hierarchy

1. Screen canvas: `background` (`#000000`).
2. Persistent chrome and focused editor regions: `surface` (`#0A0A0A`).
3. Nested controls and secondary groups: `surfaceVariant` (`#121212`).
4. Separation: `outlineVariant` border or divider; **not** shadow or tonal elevation.

Cards, overlays, dialogs, dropdowns, and plugin panels must be flat: `shadowElevation = 0.dp` and `tonalElevation = 0.dp` unless a platform component cannot expose that setting.

## Performance rules

- Do not add gradients, blur, crossfades, animated visibility, animated layout, shimmer, or visual effects solely for decoration.
- Do not use bitmap/image transformations for interface chrome.
- Preserve lazy lists for plugin, recording, and tone collections.
- Keep high-frequency audio meter state scoped to the smallest composable that renders it.
- Prefer a solid color state change over an animation.

## Screen conventions

- **Rack:** flat top/bottom chrome; compact track, recording, WAV, and plugin controls on the 8 dp grid. Plugin cards are outlined editor sections.
- **Browser, recordings, settings, TONE3000, VST:** use the shared surface hierarchy; list items are flat outlined sections with 12 dp outer padding.
- **WebView/X11/native-editor hosts:** surrounding Compose chrome must be true black and use the shared controls; do not overlay glow or gradients.
- **System bars:** black with light icons.

## Change checklist

Before landing UI work, verify:

1. New colors are `MaterialTheme` roles or documented semantic meter colors.
2. Touch targets are at least 44 dp.
3. Layout follows the 8 dp grid and does not add gratuitous elevation.
4. No new animation, gradient, blur, or expensive image effect was introduced.
5. The affected Android variant compiles; when a device is available, inspect the changed screen on-device.
