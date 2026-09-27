# ThreadFilm transfers — layout mismatch gotcha

> Root-cause note for the RTPATHOCL Metal `EnqueueWriteBuffer` crash
> (Blender GPU viewport crash, `luxball.blend`, Sep 2026).

## The bug

`PathOCLBaseOCLRenderThread::ThreadFilm::SendFilm()`/`RecvFilm()`
upload the CPU-computed engine-film channels `CONVERGENCE`, `NOISE` and
`USER_IMPORTANCE` into tile-shaped device buffers:

```cpp
intersectionDevice.EnqueueWriteBuffer(
    channel_CONVERGENCE_Buff,
    CL_FALSE,
    channel_CONVERGENCE_Buff->GetSize(),          // device-buffer size
    engineFilm->channel_CONVERGENCE->GetPixels());// engine-film pixels
```

The size comes from the **device buffer** (thread-film layout) but the
source is the **engine film** channel. The two layouts are not always
identical:

- `TilePathOCLRenderEngine::InitTileRepository()` rounds the tile width
  **up** to a multiple of `rtpath.resolutionreduction` (default 4):
  a 1915-wide film gets `tileWidth = 1916`, so every thread film and
  device buffer is 1 column wider than the engine film.
- TILEPATHOCL threads cover tile **sub-rectangles** of the film, so the
  tile origin must be applied too (previously the upload copied the
  film *origin* region regardless of which tile was being worked on —
  memory-safe only because the film is larger, but wrong data).

Result for `1915xH`, reduction 4: each upload `memcpy`s
`1916*H*4` bytes out of a `1915*H*4` host buffer — a ~4.5 KB heap
over-read per channel, per `RecvFilm`, plus row-stride misalignment on
every row after the first. On Metal the "transfer" is a plain host
`memcpy` into shared storage, so it faults inside
`MetalDevice::EnqueueWriteBuffer()` — the observed crash. The same heap
damage explains the scattered secondary crashes (`Properties::Set`
mutex init, `Film::AddSampleResultColor`, malloc-zone assertions) seen
in earlier `.ips` reports: an OOB read is not itself a write, but the
repeated kernel-visible garbage plus intermittent segfaults were the
visible signature; Guard Edges caught it on the very first transfer.

## The fix

`ThreadFilm::WriteEngineFilmChannel()` now repacks the tile overlap
row-by-row into the thread film's own channel (which has exactly the
device-buffer layout) and uploads that:

```cpp
float *dst = stagingChannel->GetPixels();        // tileW x tileH
copyWidth  = Min(tileW, engineW - tileX);
copyHeight = Min(tileH, engineH - tileY);
for (y < copyHeight)
    memcpy(dst + y*tileW,
           src + (tileY+y)*engineW + tileX, copyWidth*4);
EnqueueWriteBuffer(buff, CL_FALSE, buff->GetSize(), dst);
```

- When `tileX == tileY == 0` and the dimensions match (PATHOCL,
  multi-device slices that happen to share the film width) the flat
  fast path is kept.
- `RecvFilm`/`SendFilm` take `tileX/tileY` parameters; the tile engine
  passes `tileWork.GetCoord()`, everyone else defaults to `(0, 0)`.
- Padding columns keep the thread channel's init values
  (CONVERGENCE/NOISE = `inf`, USER_IMPORTANCE = `1`); they are never
  merged back into the engine film (`TileRepository::NextTile` clamps
  `AddFilm`/`SetFilm` to `coord.width`, which is film-bounded).

Defense in depth: `MetalDevice::EnqueueReadBuffer`/`EnqueueWriteBuffer`
now throw on `size > buff->size` — the shared-storage memcpy had no
destination-capacity check at all (OpenCL gets this from the driver).

Also fixed while here: `Tile::TileCoord`'s ctor set `height(w)` instead
of `height(h)` — currently harmless (callers only read `x`/`y`), now
correct.

## Related pre-existing hazards (not fixed here)

- `RenderSession::Parse()`'s non-resize branch runs `film->Parse(props)`
  in place while render threads may still hold channel pointers from
  the previous configuration — channel add/remove on the engine film
  races with `engineFilm->channel_*` dereferences in the render
  threads. Keep an eye on props edits mid-render.
- `VulkanDeviceDescription::AddDeviceDescs` leaks the `VkInstance`
  (no `vkDestroyInstance`) — seen as flakes in the unit-test suite.
- `Properties::operator=(Properties&&)` had `props = std::move(props)`
  (self-move, no-op) — fixed in the same commit.

## Regression test

`pyunittests/.../basic/testrtpathoclfilmresize.py ::
test_padded_tile_width_engine_channel_uploads` runs RTPATHOCL at
`1915x240` with `rtpath.resolutionreduction = 4` and all three
CPU-computed channels enabled via `film.outputs` — before the fix the
first `RecvFilm` over-read ~4.5 KB per channel.
