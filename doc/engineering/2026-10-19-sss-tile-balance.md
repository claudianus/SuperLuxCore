# CPU SSS tile balance: compatible candidate, incomplete timing gate

This closes the current investigation at the user's request. It does not
claim that the remaining Cycles raw-convergence gap has been solved.
No tile defaults or physical transport settings were changed.

Evidence: `../test-scenes/validation-2026-10-19/sss-tile-balance/`.
The immutable measured module identity is SHA256
`5948f1b3252f04ff06859a6c55b537a09eaa4a54109ecca9da2a60f71701e7a6`.
Its consumed import overlay has been removed; the build artifact, source,
native identity, raw images and receipts remain. The earlier immutable
`sss-bidir-measure/rrfree-package` is retained as one control, not a second
installed Blender provider.

## Actual render checks

The authored scene fingerprint is
`d913ff75bc07a85ff0d4d70f196668558909667d40d188d5291ea628cb16816c`.
At 320x180, RGB, seed17 and exact 144 SPP, tile widths 8, 16 and 32 produce
bit-identical raw RGB, sample-count and ray-count buffers. Every run retains
the `sss-unregularized-v2` zero-clamp/zero-regularization/no-denoising
contract. The ray-count mean is 1013.0172569444444.

The attempted CPU4 ABBA sequence completed only the first 32-pixel run.
A user-owned Blender launched before the second row; process admission
stopped the driver, and that Blender was left untouched. Subsequent runs
use CPU1 for functionality only. No ABBA ratio, multicore speedup or
equal-time Cycles result can be inferred from these runs. All owned render
children are terminal and reaped.

`dev-tools/sss_export_benchmark.py --tile-size N` now provides an explicit
repeatable tile-width/height override for a future comparison. It rejects
sizes below the renderer's minimum 8 and sets scalar/x/y properties together
so pre-existing axis overrides cannot silently defeat the request. The
default command retains the source scene's tile settings. The final writer
itself was rendered with `--tile-size 16 --threads 1`; all three saved buffers
also match the 32-pixel baseline exactly. Its request and effective config
are retained with the receipt.

## Scheduling diagnostic, not timing

`simulate_ray_work.py` uses the actual ray-count image and the renderer's
stable Hilbert tile order. Each tile is assigned to the next available
worker. The assumed cost is its sum of ray counts. It ignores different
ray/material costs, task and film-merge overhead, preparation, and CPU core
heterogeneity; its efficiency values are not measured CPU utilization or
predicted speed ratios.

| Tile width | Ray-work efficiency, 4 workers | Ray-work efficiency, 18 workers |
| --- | ---: | ---: |
| 8 | .9992 | .9629 |
| 16 | .9993 | .7480 |
| 32 | .8333 | .4026 |
| 128 | .5942 | .1321 |

This makes smaller CPU tiles a concrete follow-up candidate, especially
when few expensive SSS tiles dominate a large image. It does not justify
automatically overriding an artist's tile size or changing installed-addon
defaults without an actual complete multicore comparison. The public
Blender tile property still defaults to 128; the matched study explicitly
uses 32. Those are different configurations and must not be conflated.

The larger SSS objective remains unfinished. The dedicated PATHCPU loop,
transport correctness repairs and opt-in bidirectional connection selector
are preserved in their existing commits. Rejected guiding experiments stay
out of defaults. The current work stops here without claiming production
Cycles parity, a new installed build, or exhaustion of possible optimizations.
