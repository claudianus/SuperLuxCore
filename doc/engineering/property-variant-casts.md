# Property variant conversions — the `Get<float>` lexical_cast trap

## Symptom

`pysuperluxcore.RenderSession(...)` aborts with

    RuntimeError: bad lexical cast: source type value could not be interpreted as target

while the *same* configuration loaded from a `.cfg`/`.scn` file works.

## Root cause

`PropertyValue::Get<T>` converts stored variants via `boost::lexical_cast<T>`
(`src/luxrays/utils/properties.cpp`, helper `ret<T>`). For **numeric →
numeric** casts boost requires an *exact* roundtrip, so

    lexical_cast<float>(0.03)          // double -> float
    // float(0.03) != 0.03  =>  bad_lexical_cast

The value path matters:

- `.cfg`/`.scn` parsing stores unquoted fields as `STRING_VAL` — `Get<float>`
  goes through `FromString<float>` (istringstream), never throws.
- API / Python callers (`pysuperluxcore.Property(name, 0.03)`,
  Blender add-on `definitions[key] = value`) store `DOUBLE_VAL` — `Get<float>`
  hit `lexical_cast<float>(double)` and threw for almost every non-trivial
  value.

`film.adaptiveerror.target = 0.03` (the new Corona-style auto-halt default)
was the first engine consumer of `Get<float>` that Blender actually fed a
non-representable double. `Get<float>` on large `long long` (>2^24) had the
same latent failure.

## Fix

`ret<T>` now routes numeric→float conversions through `lcast<T,S>` =
`static_cast<float>(v)`; every other `Get<T>` keeps `lexical_cast` so the
signedness/exactness guards (`u_int` on `-1`, `int` on `8.5`) still hold.
Commit `8ff73e24b`.

## Debugging recipe that found it

1. `-E c++` lldb breakpoint fires on *caught* exceptions too — the sort
   comparator in `Properties::GetAllUniqueSubNames` deliberately throws and
   catches `bad_lexical_cast` for non-numeric subnames; keep `continue`-ing
   until a frame that is NOT the comparator appears (here:
   `Film::Parse +5540 -> PropertyValue::Get<float>`).
2. Disassembling `Get<float>` showed `fcmp d1, 0x3810000000000000`
   (=FLT_MIN) feeding `boost::throw_exception<bad_lexical_cast>` — i.e. the
   numeric-range check inside `try_lexical_convert`, not a string parse.
3. Same-value-as-string (`Property("x", "0.03")`) session succeeded,
   double variant failed -> pinpointed the variant-type asymmetry.

## Notes for API users

- `Get<float>` now behaves like `Get<double>` semantically (narrowing);
  prefer `Get<double>` in new engine code anyway — film/dataset floats are
  doubles.
- `Get<u_int>`/`Get<int>` on stored doubles only accept exact values
  (kept); emit ints from the adapter for integer-targeted properties.
