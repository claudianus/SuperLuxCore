# DLSS frame publication transaction candidate
+
+The full production frame/parameter/C API bodies reproduce ignored context-pop
+and parameter-destruction failures that returned success and published output.
+Commit output/history/count only after parameter cleanup and context restoration;
+keep caller output/raw guides unchanged on failure and poison unsafe retries.
+The SDK frame validation/evaluation/readback/alpha code is byte-identical after
+removing its former publication line. Init/resize/release/destroy are unchanged.
+
+No renderer/kernel/default/estimator change, no real NGX or RTX quality/speed
+acceptance. Built-library invalid-frame checks and Windows/manylinux2.28 ABI /
+dependency gates are not GPU frame acceptance. Published fix2 remains immutable.
+