# Restart adaptive luminance moments with an edited scene

Film samples and Sobol pixel passes restart after a scene edit, but the
per-pixel luminance first and second moments previously retained the old
scene's samples. The adaptive estimate then divided accumulated old and new
moments by only the new passes. `Film::Reset()` now clears those moments when
discarding radiance. `Clear()` retains them during repeated thread-film
merges, and the explicit counters-only reset retains its previous behavior.
No sampling strength, convergence threshold or rendering quality default
changes.

`dev-tools/film_edit_reset_contract.py` builds a temporary C++ bridge with the
actual Release flags and calls the loaded module's real sample splatter and
film methods. The earlier whole-wheel build accumulated moments, preserved
them through a merge clear, but failed to clear them on reset. The candidate
passes all three checks; both runs remove their temporary bridge.

The VDB live sequence image-difference diagnostic uncovered another test
problem: independent fresh/fresh repeats used identical seeds. Animated
frame seeds changed the fresh frame's noise pattern while the edited session
retained its initial seed, inflating the live/fresh difference relative to
the correlated repeat. The images' spatial noise did not increase fourfold.
The adapter regression now explicitly gives the repeat a different seed and
records the actual eye/light SPP and configuration. It keeps the existing
mean, pixel and block-difference tolerances. This does not establish final
convergence or broad scene compatibility.
