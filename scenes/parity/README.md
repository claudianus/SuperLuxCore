# Backend parity regression scenes

Minimal scenes that isolate a single backend behaviour so CPU / OpenCL /
Metal intersection results can be compared pixel-exactly. They are cheap to
render (`batch.haltspp = 8`, 128x128) and have closed-form expected values.

## Scenes

- `emissive-direct.scn` / `.cfg` — a directly-visible emissive quad
  (`emission = 4 4 4`, `kd = 0`) on a black `constantinfinite` environment.
  Tests the direct camera->light emission path. **Expected centre pixel:
  `(4, 4, 4)`** on every backend.

- `whiteenv.scn` / `.cfg` — an opaque matte quad (`kd = 0`, no emission)
  against a white `constantinfinite` environment. If a camera ray misses the
  quad it collects environment light, so this is a pure intersection-leak
  detector. **Expected centre pixel: `(0, 0, 0)`** on every backend.

## Running

```
luxcoreconsole scenes/parity/emissive-direct.cfg   # -> emissive-direct.hdr
luxcoreconsole scenes/parity/whiteenv.cfg          # -> whiteenv.hdr
```

Select the intersection backend with `opencl.devices.select` /
`renderengine.type`. On Apple Silicon the Metal HWRT path is the `Metal`
OpenCL device (it can be forced off with `LUXRAYS_METAL_HWRT=0` to fall back
to the software MBVH kernel on the same device).

## Regression covered

Commit `metalrtaccel`: on Apple Metal, calling the timed
`intersector.intersect(ray, as, time)` overload against a **static**
(non-motion) instance acceleration structure returned no intersection for
every ray. PATHOCL on Metal therefore leaked ~62% of camera rays to the
environment (`whiteenv` centre ≈ 0.625 instead of 0, `emissive-direct`
centre ≈ 1.48 instead of 4). The fix gates the timed overload behind
`useMotionTime`, which is only set when the instance AS was built with
`MTLAccelerationStructureMotionInstanceDescriptor` (any leaf has a motion
transform). Static AS use the untimed `intersect(ray, as)` overload, which
hits correctly; motion AS keep timed interpolation.
