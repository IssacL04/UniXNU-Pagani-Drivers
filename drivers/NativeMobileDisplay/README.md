# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

On the OnePlus 13T the CPU path is device-observed. The GPU handoff candidate is wired after GPU self-tests and retains task ownership, frame lifetime and failure/close fallback; host tests, the Run 78 production build and local image checks pass; Run 78 user observations show successful GPU presentation. Hardware display vblank is not implemented.

Linked into XNU by `unixnu build kernel`.

GPU presenter 成功计数和最后一帧 offer-to-completion 纳秒耗时供 BootProgress 计算 GPU 呈现率。该时间包含用户态交接、映射与等待，不是 GPU shader 执行时间；CPU 回退另计。Run 78 用户已观察到 518 次成功 GPU 呈现、0 次失败，准确帧率和长期稳定性仍待测试。

The presenter retains up to six mappings for the admitted task and returns stable IDs to the Metal import cache; overflow remains transient. Close and mapping failure release all entries. Optional overlay negotiation reserves diagnostic rows in GPU blits and CPU copies/fills. Mapping creation/hit counts and host regressions cover reuse, ownership, overflow and cleanup.

GPU handoff accepts legacy 16-byte completions and optional 40-byte timing completions. Owner, sequence, input length and reserved fields are validated before completion. The last successful frame exposes offer-to-delivery, dependency wait, encode/import, submit, completion wait and runtime GPU timing; software VSYNC notification/deadline counters remain distinct from physical panel timing.
