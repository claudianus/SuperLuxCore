# Archive-root records: `.flm` / `.rsm` / `.rst` / config round-trip repair

Boost serialization writes a *per-type* record for the root object. The
record layout differs between `ar << obj` (by value), `ar << T*` (raw
pointer), `ar << unique_ptr<T>` and `ar << shared_ptr<T>` — and a loader
must read exactly the type that was written. `ar << unique_ptr<T>` puts
a version-0 wrapper around a nested `T*` record, so the root version
field is 0; `ar << T*` puts `version<T>` (e.g. Film = 30) in that field.
`ar >> unique_ptr<T>` on a `T*` stream therefore throws
`unsupported_class_version` ("class version unique_ptr<...>").

Every save/load pair must be symmetric. The broken pairs (all
upstream-inherited, unreachable since the paths were written):

| File | Save wrote | Load read | Status before |
|---|---|---|---|
| `.flm` (`Film`) | `Film&` by value | `unique_ptr<Film>` | always threw |
| `.cfg` (`RenderConfig`, ConstRef overload) | `RenderConfig&` by value | `unique_ptr<RenderConfig>` | always threw |
| `.cfg` (`RenderConfig`, UPtr overload) | `unique_ptr` | `unique_ptr` | worked |
| `.rst` (`RenderState`) | `RenderState*` | `shared_ptr` | always threw |
| `.rsm` entry 1 (config) | `RenderConfig&` by value | `unique_ptr` | always threw |
| `.rsm` entry 2 (state) | `shared_ptr` | `shared_ptr` | worked |
| `.rsm` entry 3 (film) | `unique_ptr` | `unique_ptr` | worked |
| `.bcf` (`Scene`) | `unique_ptr` | `unique_ptr` | worked |

## Convention now enforced

All archive roots are raw `T*` records on **both** sides:

- Saves write `objPtr` (`film`, `renderConfig.get()`, `&renderConfig`,
  `renderSession->film.get()`) — never a non-owning `unique_ptr`, which
  would need an exception-unsafe `release()` guard.
- Loads read `T*` then adopt into the owning type:
  `FilmUPtr(f)`, `RenderConfigUPtr(rc)`, `RenderStateSPtr(rs)`,
  `unique_ptr<Film>(sf)` — with a null check on each.
- `.rsm`'s `shared_ptr<RenderState>` entry is untouched (symmetric pair).
- `Scene`'s `unique_ptr` pair is untouched (symmetric pair).

Class-version semantics are preserved: under both wrappers the pointee's
own `version<T>` reaches `T::load(ar, version)` unchanged.

## pyluxcore fixes needed to reach the code

- `RenderConfig.LoadResumeFile`: `py::make_tuple(config, state, film)`
  stored the `unique_ptr`s and the tuple caster forwarded them as
  `const unique_ptr&` → "Invalid return_value_policy". Cast each
  element explicitly (`py::cast(std::move(ptr))`).
- `RenderSession(config, state, film)`: ctor bound to
  `FilmImplStandalone&`, an unregistered type — dead binding. Widened
  to base `FilmImpl&` (the ctor only calls virtual `GetSLGFilm()`).
  The binding also needs `py::keep_alive<1, 4>`: the engine stores the
  start film as a non-owning `FilmPtr` (observer_ptr) and dereferences
  it in `RenderEngine::Start()` via `film->AddFilm(*startFilm)` (on
  every start, including Stop→Start cycles), so the Python film object
  must outlive the session. `state` is a copied `shared_ptr` and needs
  no keep_alive; `config` already had `keep_alive<1, 2>`.

## Start-film lifetime (same defect class, impl side)

`RenderSessionImpl(config, stateFile, filmFile)` loaded the film into a
ctor-local `unique_ptr` and passed `FilmPtr(startFilm.get())` to the
session — the film was destroyed at ctor exit, leaving the engine's
`startFilm` dangling → UAF in `Start()`. The loaded film is now owned
by a `RenderSessionImpl::resumeFilm` member (declared before
`renderSession` so it outlives it on teardown). The same guarantee for
the object-based binding comes from `keep_alive<1, 4>` above; the C++
`luxcore::RenderSession::Create(config, state, film&)` API keeps its
caller-owns contract.

## Loader hardening

`RenderConfigImpl`'s `.rsm` ctor null-checks every archive entry:
config via `ReadFromSIF<T>` (`t == nullptr` → throw), and the state +
film entries explicitly. A null pointer record is a corrupt file, not
a stream error — `IsGood()` alone does not catch it, and a null film
otherwise surfaces later as a `GetSLGFilm()` null-deref.

## Verification

`dev-tools/e94_serialize_roundtrip_test.py`: renders with adaptive
error enabled, then `.flm` save → `Film(path)` load (NOISE channel
finite → `adaptiveError` rebound to the loaded film via `BindFilm`),
`.rsm` save → `LoadResumeFile` → resumed session keeps accumulating
samples (with the Python film object dropped before `Start()` to
exercise the binding's `keep_alive`), plus the `.rst` + `.flm`
filename-resume path (`resumeFilm` ownership). PASS: 3/3.
