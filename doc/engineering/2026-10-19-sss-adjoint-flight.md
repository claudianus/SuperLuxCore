# CPU SSS flight cost and adjoint boundary correctness

The CPU SSS walk sampled a phase direction before constructing its guided proposal, then overwrote that sample on every guided step. The dedicated loop now samples the original BSDF only when guiding is unavailable, and samples the classic branch once when the mixture chooses it. The random dimensions, physical phase, proposal probabilities, roulette, depth and boundary handling are unchanged.

The flight estimator also evaluated three physical transmittance exponentials independently from its nine-component mixture. When a classic component rate equals the physical extinction exactly, the estimator now reuses that double-precision exponential. The mixture is summed in its original order. A different source rate, including a textured flight, retains its own exponential. This reduces 12 exponential calls to 9 on a matching guided flight. It does not alter or approximate extinction.

Before adding more light-side sampling, the ordered OpenPBR SSS boundary operator was tested in both traversal directions. Direction-dependent energy compensation changed the value for the same ordered light/eye pair by up to 43.7823%, including 20.6433% for reflection. Shared-frame SSS evaluation now keeps the established camera-side physical operator while retaining the light-side proposal PDFs. The tested CPU and device kernels use the same rule. The device patch depends on pre-existing public material/PDF changes in the shared worktree; its owned delta is retained in `owned-production.patch` rather than committing another task's prerequisite edits. Device source remains pending integration. The device shared-frame SSS sampler also uses the actual ordered directions and forward PDF on a light ray. Independent lobe normals retain their existing path and are outside this repair.

## Verification and limitations

Evidence lives in `test-scenes/validation-2026-10-19/sss-exit-nee/` in the workspace parent. The exact standalone core export was first checked bit-for-bit against the stock Blender raw film. This permits cheap proposal screening without changing authored geometry or material parameters; it does not replace installed-addon checks.

- 108 ordered RGB boundary cases: physical value matches exactly between traversal endpoints. Original camera values and forward densities remain unchanged in the original capture.
- Real Metal device, 32 evaluation and sampling cases: compare public material stack operations against CPU with the classic boundary proposal selected on CPU, since the device retains that proposal. Relative maxima: energy 3.54e-6, public PDF 6.95e-7, sampled value 3.22e-6, sampled direction 2.33e-6, sampled PDF 4.51e-6. Nonzero events agree; stacks are empty. Existing public black-return event conventions differ on six rejected eye BTDF directions.
- 14 CPU fixtures at 256 exact samples: all 112 RGB/AOV/count buffers remain bit-exact against the previously delivered boundary library. Fixtures include textured, anisotropic, nested, shadow-transparent, spectral and finite-area cases.
- The full artist fixture is also tested in RGB and spectral modes. See the final image/readback JSON for actual outcomes rather than treating this document as a substitute for receipts.

The initial ABBA timing screen was contaminated by unrelated CPU/WindowServer activity. Its provisional speed impression is withdrawn. No speed ratio, equal-time noise reduction, Cycles parity or production quality claim follows from that screen. The two eliminated computations are structural improvements; a clean matched performance measurement remains required.

## Rejected experiments

Extra finite-area boundary NEE was implemented and screened as three estimators: a separate two-step probe, a rotated light-axis probe, and cached global boundary-hit variants, including exclusive terminal-path ownership. The 3-technique MIS algebra and front-lit mean were checked, but costs outweighed error reduction; exclusive ownership worsened raw noise. No variant was adopted. Production traversal changes and observer state were removed; replayable source snapshots, patches, image buffers, hashes and process receipts remain in the evidence folder. Consumed package overlays were deleted.

These probes collapse only the final flight. They are not a full implementation of subdivision next-event estimation, which inserts another volume scattering vertex. The Cycles source inspected is the local Blender 5.2 tree at d13f752e3b9c4f8c261cda552b1021f8bcc0382c, matching the installed Blender version. Its nonlocal BSSRDF and diffuse exit differ from this renderer's refractive bulk path. Preserving the latter requires evaluating sampling changes under the latter's own reference; copying the entire Cycles walk would change the target model.

The broad SSS convergence goal remains active. Next substantial work must target a better path-space estimator or validated boundary/volume connection strategy. Parameter sweeps of the rejected final-flight estimator are not justified by this evidence.
