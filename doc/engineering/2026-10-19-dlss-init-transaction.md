# DLSS initialization transaction candidate over published fix2

Reproduces context ownership loss after initial pop failure, ignored final init
pop failure, rollback context-destroy/NGX-shutdown failure, and allocation failure
while publishing the initialized manager. Own the context until successful
publication; preallocate host strings, check pops, attempt own-context rollback,
retain failed-destroy identity and poison reuse after cleanup failure.

Only separate helper initialization changes. ABI, frame processing, resize,
manager release, destroy, renderer source and kernels remain unchanged. Windows
and Linux mock-driver tests plus official-SDK helper compile/ABI are scoped
evidence, not actual RTX/NGX evaluation, quality, speed or tester crash acceptance.
Published fix2 assets stay immutable; this candidate is not installed or released.
