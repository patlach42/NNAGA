- Уменьшать playback watermark. На
Audient минимальный steady_target сейчас 336 frames — это уже около 7.0 ms при 48 kHz до остальных очередей. Нужен per-device калиброванный минимум, а не один консервативный target для всех интерфейсов.
- Уменьшать packets-per-transfer/число заранее отправленных USB transfers.
Это снижает hardware queue, но повышает wake-up rate и риск xrun. Только через device calibration.
- RT affinity, pre-touch, sustained-performance/DVFS control. Это
уменьшает jitter и позволяет безопаснее снизить watermark