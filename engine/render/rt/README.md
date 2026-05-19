# engine/render/rt — M5 hardware RT work

This directory implements hardware ray-tracing for Psynder-GX per DESIGN §7.4–§7.6: RT-accelerated sun/dynamic-light shadows (§7.4), a DDGI probe grid for indirect lighting (§7.5), and hybrid SSR+RT reflections (§7.6). All entry-points are currently Wave B no-op stubs; full hardware-RT dispatch (shader binding tables, denoiser passes, DDGI probe atlas) lands at milestone M5. Until then, lane 09 (render-pipeline) owns the CSM shadow fallback path that runs on hardware without ray-tracing capability (Apple Silicon < M3, AMD < RDNA2, NVIDIA < Turing).
