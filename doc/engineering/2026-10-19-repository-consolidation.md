# Repository consolidation and storage maintenance — 2026-10-19

The workspace contains independent Git repositories. This maintenance integrates
the existing renderer, adapter and validation work into their local `main`
histories without treating historical release snapshots as newer source.

## Integration decisions

- Initial renderer `main` was 16 commits ahead of `origin/main`, with 96 modified
  tracked files and 74 untracked source/test/document files. Adapter `main` was
  five commits ahead, with 26 modified tracked files and 118 untracked files.
  Complete changes were committed before merging. Small original patches and
  untracked-source archives remain in the workspace maintenance receipt folder.
- The renderer fetch refspec previously selected only `main`. Fetching all
  remote heads exposed 12 release/review branches; the adapter had five. Their
  tips, together with both dirty Phase72 validation worktrees, are ancestors of
  the consolidated component mains.
- The existing CUDA teardown and DLSS runtime sources already contained the
  released fixes. Current Phase72/79 camera/readback/film-data changes and CPU
  SSS work supersede the earlier Phase70 release snapshot. The renderer's
  `include/`, `src/` and `python/` trees remain identical to checkpoint
  `0ad1d146a`; adapter production Python contents and stable manifest remain
  those of checkpoint `01823295`.
- Preserve detailed current engineering reports, import missing release
  provenance and packaging tools, and update the frozen package scripts to
  fix4. Retain the generic multi-platform wheel builder. Preserve the separate
  fix4 acceptance workflow as `dlss-fix4-acceptance.yml` instead of replacing
  that generic builder with a historical single-purpose release workflow.
- The two Phase72 worktrees were removed only after committing their changes
  and verifying ancestry from main. The accepted packaged native and its debug
  symbols were retained separately; the unstripped rebuildable native and
  obsolete build directory were retired. The renderer now fetches all heads.
- Local MoltenVK `vkrt` work and its pending logging Makefile change were merged
  into local MoltenVK `main`, also retaining the upstream indirect/tessellation
  changes. clspv's two local commits were already on main. External upstream
  repositories are not push destinations for these local experiments. Nested
  dependencies retain their selected detached revisions; notably SPIRV-Cross
  HEAD `69472361` includes four published fork fixes beyond manifest revision
  `6c153db3`. The receipt records exact dependency HEADs rather than resetting
  them to unrelated upstream main revisions.

## Verification and publication boundary

The native Release module was rebuilt using
`ninja -C out/build -f build-Release.ninja -j 2 pysuperluxcore`.
The resulting module SHA256 is
`5c7cb939d61231d992e5ff38ec730baff4345b8887143bf75c79712dc8a137c2`.
Existing compiler override/duplicate-library warnings remain; the build passed.

Fourteen adapter host regression scripts and sixteen core lifecycle, cleanup,
DLSS fault and viewport/bounds scripts passed. CPU edit/resume/orthographic
native checks passed. Actual CPU and explicitly selected Metal GPU image
pipeline replacement/cleanup checks preserved raw RGB bit-for-bit, with two
GPU records and no hardware-plugin fallback. The seven-case viewport Stop
ownership regression and real wheel CLI/RECORD/cache regression passed.
Initial missing test dependencies and sandbox GPU-discovery failures are
retained separately from the successful final receipts. The Metal path-tracing
edit check has its own terminal receipt and is not inferred from the image
pipeline test.

This is repository maintenance and bounded regression evidence. It does not
complete the outstanding RTX normal-GUI, NGX quality/performance, heavy viewport,
SSS convergence or new installed-addon product gates. Experimental DLSS remains
explicit; stable manifest identity remains 2.11.34. The final documentation
commit uses `[skip ci]` because this repository automatically publishes rolling
`wheels-latest` after main CI. Source synchronization must not silently replace
public native packages with a combined work-in-progress build.

## Storage policy and receipts

Retire obsolete shader/editor/compiler caches, disposable isolated installed
payloads, old local packaging wheels and unused Debug install products. Preserve
authored scenes, unique EXR/NPY/NPZ data, reports, failures, source histories and
current complete packages. No broad `git clean -xfd` or aggressive reflog/object
pruning was used.

866 identical evidence-file paths were consolidated with hard links after
SHA256 grouping and complete byte comparisons. Original paths and content
digests are preserved; these historical evidence datasets should be treated as
immutable. Modify a new copy when creating a new experiment. Unique evidence
remains, so the workspace is still large despite removing approximately 27 GiB
of redundant/generated storage.

Full local receipts: workspace
`dev-tools/maintenance/2026-10-19-main-cleanup/`. They contain initial statuses,
source preservation patches, commit/merge records, dependency HEAD inventory,
cleanup/de-duplication manifests, native/host test logs and final status/storage
measurements. Workspace-root `ROADMAP.md`, helper scripts and research evidence
remain outside the component Git repositories and were preserved in place.
