1. generic proxy 和 aysnc proxy 是两种存储访问的标签，前者指 warp 中的 threads 通过 `load/store/atom` 读写内存的方式，后者指 TMA/tensorcore 等其他硬件组件异步读写数据方式（这里的异步是相对于 threads 而言的）。两个 generic proxy 的操作之间是有序的，但是 generic proxy 和 async proxy 之间数据的一致性难以保证，所以需要 `fence.proxy.async` 来做同步。这个 `fence` 是单线程内部的 cross-proxy fence，不存在跨线程同步的作用。由于 generic-proxy 通常是以 thread 为粒度完成的，而 asynchronizing-proxy 则是某一个硬件部件独立完成的，所以单线程 fence 无法在二者之间起到 ordering 的作用，因此 `fence.proxy.async` 也必须要配 `__syncthreads( )`/`mbarrier` 等等这样跨线程同步的机制一起做。

2. 根据文档里的形式，我发现，比如都是做 64B swizzle，M/N-major 的情况下 M/N 维就可以超过 64B，比如 128B，做多轮 swizzle；但是在 K-major 的情况下 K 维却不可以超过 64B，也就是 K 维上限就是一个 swizzle 的量。所以 M/N-major 的时候需要有 LBO 的存在，但是 K-major 的时候却不需要看 LBO.

-------

> If a memory instruction does not specify a state space, the operation is performed using generic addressing. The state spaces .const, Kernel Function Parameters (.param), .local and .shared are modeled as windows within the generic address space. Each window is defined by a window base and a window size that is equal to the size of the corresponding state space. A generic address maps to global memory unless it falls within the window for const, local, or shared memory. The Kernel Function Parameters (.param) window is contained within the .global window. Within each window, a generic address maps to an address in the underlying state space by subtracting the window base.

不同的存储部分（比如 SRAM 上的 shared memory? HBM 里的 const memory? SRAM 上的 const cache? HBM 里的 local memory?）都被抽象为 global memory 中一个个 windows，其实就是一些小块区域。通常一个 generic pointer 在编译时会通过边界判定找到对应的 window，再用 generic pointer 的数值减掉 window base 得到对应存储空间内的偏移，再用特定的访存指令做数据读写。如果能在编译时完成判断（比如 PTX 指令里有 `shared::cta` 标识符），则编译后的 SASS 会直接使用特定的访存指令（如 `ld.shared` 类似的）；否则需要在运行时由硬件先做一下 window 归属判断，再做访存。注意 .param 本身属于 global 的一部分，即使被判定为 .param 之后依然按 global ptr 方式访存。

----

3. LBO 不是给 A 矩阵用的，是给 B 矩阵用的。而 SBO 对 A 矩阵数据加载和 B 矩阵数据加载都有用。

4. wgmma 的默认约定是两个操作数都是 K-major.

5. ldmatrix 数据 generic proxy. 所以 ldmatrix 之后需要有 wgmma.fence. wgmma.fence 是做 warp group 级别的执行对齐和寄存器交接，保证后面 tencore 执行前寄存器写入已经全部完成。但是 ldmatrix 目前只有 ldmatrix.m8n8.b16，只有 ldmatrix 加载之后的寄存器布局与 wgmma 计算时累加器布局一致才可以，现在只有 16-bit 的数据类型满足，比如 bf16 和 fp16 两个数据类型可以。

6. TPC 内通信比 GPC 内跨 TPC 通信要快，因为，同一 TPC：SM0 → SM1，一跳；跨 TPC：SM0 -> TPC 内部 -> GPC 交换结构 -> 目标 TPC -> 目标 SM，多跳。
