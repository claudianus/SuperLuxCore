# Metal scene edits and bounded exact blackbody evaluation

Moving or replacing a volume could briefly restore pixels and sample counts
from the previous Metal frame. GPU buffers were cleared, but the host thread
films retained their old state until the first asynchronous download.
`UpdateStats` could merge that state and immediately meet the sample halt
condition, leaving an old-position image. Scene edits now reset every host
thread film while workers are stopped, before clearing GPU films. The hybrid
native eye sampler is reset before the workers restart as well.

The actual Blender exporter/recorded replay regression covers initial render,
move/nonuniform scale, named density material edit, and disconnected Volume
output at 1280 by 720 and 64 SPP. The local whole wheel passes all four Metal
hybrid cases against fresh exports and independent fresh-repeat noise.
Object counts are 1/1/1/0; removing the volume leaves a uniform white world.
The largest live/fresh mean difference is 0.005363%. The moved case changed
from an old-position ghost with 1.4109% mean difference to the correct bounds
and 0.003334% difference. Six original images spanning all four cases were
reviewed directly. This verifies headless edits, not GUI redraw or final
production convergence. Evidence: `../test-scenes/validation-2026-10-11/`
`vdb-attributes/live-metal-native-r5/{metrics,visual-review}.json`.

Spectral blackbody evaluation previously reconstructed a 256-sample Planck
SPD and its luminance integral at every evaluation, including repeated
temperatures along a VDB ray. A thread now retains one exact-temperature SPD
and integral. There is no temperature quantization or growing temperature
map. Temperature changes replace the entry, wavelength evaluation remains
per call, and worker exit releases the entry. The original normalization
arithmetic is preserved.

`/Applications/Blender.app/Contents/Resources/5.2/python/bin/python3.13
dev-tools/blackbody_exact_cache_contract.py --output /tmp/blackbody.json`
loads the actual Release module and runs its C++ companion against an
independent copy of the old SPD/integral computation. Four workers check
6912 results across constant/textured temperatures, repeated/changed/fractional
temperatures, both normalization settings, wavelength samples and alive masks.
All float bits agree. The temporary bridge is removed on success or failure.
This is a numerical contract, with no speedup claim.

The verified local wheel has native SHA-256
`83e672468181a4dd5f5fe0e685a1f745f8a9ab1adec34a4422bd72ea4fd083f7`.
Broad Cycles compatibility, VDB edge reconstruction, volume Generated
coordinates, colored-medium transport and production scene coverage remain
open. This change does not certify 99% compatibility.
