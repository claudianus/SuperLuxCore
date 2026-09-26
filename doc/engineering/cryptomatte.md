# Cryptomatte AOVs

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Cryptomatte (`CRYPTOMATTE_OBJECT` / `CRYPTOMATTE_MATERIAL`)

Hashed-ID matte AOVs (`aabfdb3e5`). Per-pixel (id, coverage) pairs keyed
on the FIRST camera-visible surface's murmur3-float id
(`include/luxrays/utils/murmurhash.h` — `hash_to_float` keeps the sign
bit, clamps only the exponent off 0/255 per the Cryptomatte spec).

- `CryptoFrameBuffer<6>`: LEVELS (id,coverage) slots + trailing weight.
  Film merge re-inserts pairs by id — element-wise add would corrupt
  slot order across sources. GPU slot claim is lock-free
  `atomic_cmpxchg` on `cryptoObjectID`/`cryptoMaterialID`.
- Ids emitted coverage-descending with id-bits tie-break
  (deterministic). EXR: `<Name><rank2d>.RGBA` (CryptoObject00..02) +
  `cryptomatte/<key>/{name,hash,conversion}` + JSON manifest (scene-
  owned, injected as opaque film metadata).
- Recorded at first hit on eye paths (path tracer, bidir eye+connect,
  MNEE splats); misses write 0. GPU: `cryptoID` on
  SceneObject/Material/SampleResult; HitPoint carries the object id.
- Regression: `dev-tools/e40_cryptomatte_test.py` (13/13: ids, coverage
  vs alpha exact match, EXR channels/metadata/manifest, GPU parity);
  `dev-tools/e40_cryptomatte_visual.py` (720p).
- Remaining: asset-level mattes, deeper rank coverage tuning.

