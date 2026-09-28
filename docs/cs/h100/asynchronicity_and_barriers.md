---
tags:
  - NVIDIA H100
  - CUDA
  - PTX
  - 异步执行
  - mbarrier
---

# 异步执行与屏障

这篇笔记整理 NVIDIA H100 上与异步执行、TMA、`mbarrier`、Cluster Barrier、异步 Group 和 Named Barrier 有关的概念。核心问题只有两个：

1. 如何把慢速的数据搬运与快速的计算重叠起来；
2. 如何保证计算单元只消费已经准备好的数据，并且生产者不会过早覆盖仍在使用的数据。

!!! note "阅读提示"
    本文以 PTX 语义为主。指令是否可用、支持哪些限定符以及最低目标架构，应以正在使用的 PTX ISA 版本为准。

本文中的代码分为三类：

- 标注为 **CUDA C++** 的代码展示可使用的上层 API；
- 标注为 **PTX** 的代码展示指令级语义，寄存器和地址准备可能被省略；
- 标注为 **结构示例** 的代码只强调流水线控制关系，不保证复制后即可单独编译。

Hopper 专属指令应使用与目标代码匹配的 PTX 版本，并以 `sm_90` 或相应架构为编译目标。

----

<br><br>

## 1. 为什么需要异步执行

### 1.1 Doing 与 Fetching

一个计算过程通常包含两类工作：

- **Doing**：执行计算，例如矩阵乘法、求解表达式；
- **Fetching**：取得计算所需的数据，例如从 HBM 搬运到 Shared Memory。

同步执行会依次完成二者：

```text
搬运 Tile 0 ──完成──> 计算 Tile 0 ──完成──> 搬运 Tile 1 ──完成──> 计算 Tile 1
```

如果数据搬运远慢于计算，执行单元会花大量时间等待。异步执行把“发出请求”和“取得结果”解耦，使下一份数据的搬运能够与当前数据的计算重叠：

```text
时间 ───────────────────────────────────────────────>

搬运单元:  [搬运 Tile 0][搬运 Tile 1][搬运 Tile 2]
计算单元:               [计算 Tile 0][计算 Tile 1][计算 Tile 2]
```

这就是 **Latency Hiding**。如果取得下一份资源的时间高于处理当前资源的时间，就应尽可能重叠二者。

----

<br>

### 1.2 阻塞与非阻塞

阻塞操作在完成前不会把控制权交还给发起线程。非阻塞操作只负责发起任务，随后立即返回；任务由其他硬件单元在后台推进。

GPU 异步操作通常包含三个阶段：

1. **发起**：线程发出异步指令，随后继续执行；
2. **跟踪与并行执行**：硬件在后台执行操作，并维护完成状态；
3. **同步**：消费者在真正使用结果前确认操作已完成。

例如：

- TMA 使用 `mbarrier` 跟踪某些传输的完成状态；
- WGMMA 使用自身的异步提交与等待机制；
- `cp.async.bulk` 的部分形式使用 `bulk_group` 和 `wait_group`。

----

<br>

### 1.3 H100 上的矛盾

Warp 发出指令很快，而从 HBM 搬运数据到片上存储需要更长时间。因此程序同时面对两项要求：

- **重叠搬运与计算**，避免 Tensor Core 空等；
- **建立正确依赖**，避免计算读取尚未到达或已经被覆盖的数据。

异步编程的难点不在“发起任务”，而在正确表达完成条件、内存可见性和缓冲区生命周期。

-----

<br><br>

## 2. CUDA 内存模型中的 Proxy

### 2.1 Proxy 是访问机制的分类

在 CUDA/PTX 内存模型中，Proxy 表示一种内存访问机制，而不是某个线程或某一级缓存的别名。

| Proxy | 典型访问方式 |
| --- | --- |
| Generic Proxy | CUDA 单个线程执行普通 load/store |
| Async Proxy | TMA、`cp.async.bulk` 等异步数据搬运 |
| Tensormap Proxy | 访问或修改 Tensor Map |
| Texture / Surface Proxy | 纹理和 Surface 访问 |

同一地址由相同的 Proxy 先后访问时可以按程序顺序保证数据的 **可见性** 与 **正确性**。同一地址先后由不同 Proxy 访问时，不能仅凭程序顺序假设两次访问已经建立跨 Proxy 的顺序和可见性，**程序必须使用对应的同步机制**。

!!! warning "不要把 Proxy 简化成缓存路径"
    “Generic Proxy 走 L1、Async Proxy 绕过 L1”可以帮助形成直觉，但不是 Proxy 的定义，也无法覆盖 Shared Memory 场景。Proxy 是内存一致性模型中的抽象访问机制；是否需要同步取决于内存模型，而不是一张固定的缓存路径图。

-----

<br>

### 2.2 跨 Proxy 的 RAW 与 WAR 风险

#### 2.2.1 Read After Write Hazard

Generic Proxy 先写入某个地址，Async Proxy 随后读取同一地址。如果没有建立跨 Proxy 顺序，异步操作不保证观察到新值：

```text
Generic Proxy:  写 X
Async Proxy:    读 X    // 可能没有看到上面的写入
```

典型场景是线程写好 Shared Memory 后，让 TMA 从 Shared Memory 写回 Global Memory，这时仅默认用程序顺序会出错。

#### 2.2.2 Write After Read Hazard

Async Proxy 仍在读取缓冲区时，Generic Proxy 已开始覆盖同一缓冲区：

```text
Async Proxy:    读取 Tile K
Generic Proxy:  覆盖为 Tile K+1    // 过早复用缓冲区
```

这会破坏正在进行的传输或计算。解决办法是等待异步操作已经读完源缓冲区，再允许生产者复用它，barrier 在这里就很有必要了。

---

<br>

### 2.3 Ordinary Fence 与 Proxy Fence

普通 fence 用于约束某个作用域内的内存顺序。常见语义包括：

- **release**：release 之前的访问不能被推迟到它之后 $\rightarrow$ 从上游的角度等价于释放锁；
- **acquire**：acquire 之后的访问不能越过它提前执行 $\rightarrow$ 从下游的角度等价于获取锁。

作用域限定哪些线程可以直接参与这次同步关系。PTX 中的四个主要作用域由小到大为：

| PTX 作用域 | 覆盖范围 | 典型使用场景 |
| --- | --- | --- |
| `.cta` | 当前 CTA，也就是当前 Thread Block 内的所有线程 | 同一 Block 内通过 Shared Memory 协作 |
| `.cluster` | 当前 Thread Block Cluster 中的所有线程，包括属于其他 CTA 的线程 | H100 Cluster、Distributed Shared Memory、TMA Multicast |
| `.gpu` | 当前 CUDA 程序中、同一 GPU 上的线程，也包括主机在同一设备上启动的其他 kernel grid | 同一 GPU 上跨 CTA、跨 Grid 的 Global Memory 通信 |
| `.sys` | 整个程序系统中的线程，包括所有参与的 GPU 和 Host CPU 线程 | CPU-GPU、Multi-GPU 或系统级共享内存通信 |

例如，Thread Block A 写入一段 Global Memory：

- 只有同一个 Block 的线程需要观察该写入，`.cta` 足够；
- 同一 Cluster 中另一个 Block 需要观察，至少使用 `.cluster`；
- 同一 GPU 上任意 Grid/CTA 需要观察，使用 `.gpu`；
- Host 或其他 GPU 也需要参与同步，使用 `.sys`。

**PTX：同一种 fence 的不同作用域**

```ptx
fence.release.cta;      // 发布给当前 CTA
fence.release.cluster;  // 发布给当前 Cluster
fence.release.gpu;      // 发布给当前 GPU
fence.release.sys;      // 发布到系统范围
```

所以：

!!! important "Scope 不是缓存操作，也不是集合屏障"
    `.cta`、`.cluster`、`.gpu` 和 `.sys` 描述的是同步关系允许覆盖的线程集合。它们不表示“把数据写到 L1/L2”，也不会仅凭一个 fence 就让范围内所有线程停下来会合。要建立完整的跨线程通信，通常还需要另一端执行匹配的 acquire、atomic 或 barrier 操作。

!!! note "选择满足正确性的最小作用域"
    作用域越大，潜在同步成本通常越高。只在一个 Block 内共享数据时，不要无理由使用 `.gpu` 或 `.sys`。另外，具体指令未必支持全部四种 scope；例如本文的某些 `mbarrier` 形式只提供 `.cta` 或 `.cluster`。

跨 Proxy 访问同一内存位置时，还需要相应的 Proxy Fence。例如 Generic Proxy 写 Shared Memory、Async Proxy 随后读取时，本文使用显式的单向形式：

```ptx
fence.proxy.async::generic.release.sync_restrict::shared::cta.cluster;
```

这条指令可以拆成六部分：

| 组成部分 | 含义 |
| --- | --- |
| `fence` | 建立内存访问顺序 |
| `.proxy` | 顺序发生在不同的内存访问 Proxy 之间 |
| `.async::generic` | 使用 `to::from` 语法，明确表示 Generic → Async |
| `.release` | 发布 fence 之前的 Generic Proxy 内存操作 |
| `.sync_restrict::shared::cta` | 只约束当前 CTA Shared Memory 中的对象 |
| 最后的 `.cluster` | 该单向 fence 的同步 scope；这是 `sync_restrict::shared::cta` 形式要求的 scope |

定向 Proxy Fence 使用：

```text
to_proxy::from_proxy
```

因此：

```text
async::generic = Generic Proxy -> Async Proxy
```

还要注意这条指令中存在两个不同层次的 `cta/cluster`：

```text
fence.proxy.async::generic.release.sync_restrict::shared::cta.cluster
                                                  └─────────┘ └─────┘
                                                   地址空间    同步 scope
```

`shared::cta` 是完整的**地址空间限定符**，表示当前 CTA 自己的 Shared Memory；末尾的 `.cluster` 才是同步作用域。指令格式要求使用 `.sync_restrict::shared::cta` 时指定 `.cluster` scope，但这不会把当前 CTA 的 Shared Memory 变成 Cluster Shared Memory。

因此，这条 Proxy Fence 只负责排序调用线程的内存操作。用在 Shared-to-Global TMA 之前时，它建立如下顺序：

```text
当前线程通过 Generic Proxy 写 Shared Memory
                    ↓
fence.proxy.async::generic.release.sync_restrict::shared::cta.cluster
                    ↓
后续 TMA 通过 Async Proxy 读取 Shared Memory
```

它不会等待其他线程到达，也不会等待 TMA 完成。若多个线程共同写入 Shared Memory，每个生产线程都应在自己的写入后执行 Proxy Fence，再通过 CTA 级同步保证 elected thread 不会提前启动 TMA：

```cpp
smem[threadIdx.x] = value;          /* 每个线程通过 Generic Proxy 写入 */

/* 每个写入线程发布自己的 Generic -> Async 顺序 */
fence_generic_to_async_shared_cta();
__syncthreads();                    /* 等待整个 CTA 完成写入和 Proxy Fence */
if (is_elected()) {
    launch_tma_store();             /* 只有完成 CTA 会合后，才启动读取这块 Shared Memory 的 TMA */
}
```

CUDA C++ 的 `cuda::ptx::fence_proxy_async(cuda::ptx::space_shared)` 对应传统的双向形式。为了明确表达这里所需的 Generic → Async 方向，可以用 inline PTX 封装单向指令：

**CUDA C++：封装 Generic → Async Proxy Fence**

```cpp
__device__ __forceinline__ void fence_generic_to_async_shared_cta( ){
    asm volatile(
        "fence.proxy.async::generic.release."
        "sync_restrict::shared::cta.cluster;"
        ::: "memory"
    );
}

__device__ void publish_tile(float* smem, float value)
{
    smem[threadIdx.x] = value;          // Generic Proxy 写 Shared Memory
    fence_generic_to_async_shared_cta();
    __syncthreads();                    // 等待 CTA 内所有写入者

    if (threadIdx.x == 0) {
        // 此后才由 elected thread 发起 Shared -> Global TMA
    }
}
```

这里的 `"memory"` clobber 约束编译器，而 PTX fence 约束设备内存模型；它们解决的层次不同。

----

<br><br>

## 3. mbarrier 的状态模型

### 3.1 mbarrier 是什么

`mbarrier` 是驻留在 Shared Memory 中的 64-bit 硬件同步对象。它既能跟踪线程到达，也能跟踪异步事务的完成。

可以把一个 phase 的完成条件概括为：

```text
pending arrival count == 0 && pending transaction count == 0
```

它适合构造 split-phase barrier：生产者先发起工作，消费者稍后检查完成状态，中间可以执行不依赖结果的任务。

---

<br>

### 3.2 Arrival Count

初始化时指定 **expected arrival count**. 当前 phase 中，每次有效 arrive 会减少 **pending arrival count**. 当 barrier 进入下一 phase 时，pending arrival count 会依据 expected arrival count 重新建立。

它可以表达“需要多少参与者完成某个阶段”。参与者不一定是所有线程，也可以是有限数量的 producer/consumer 线程。

----

<br>

### 3.3 Transaction Count

异步数据传输还可以给 barrier 增加待完成的**事务字节数**（expected transaction bytes）。它不是线程数，也不是异步指令的条数，而是当前 phase 承诺还会由异步操作完成的数据量。单位是 byte。

`expect_tx` 相当于先给 barrier 记一笔“尚未完成的异步工作”：

```text
登记异步操作：pending transaction count += expected bytes
异步操作完成：pending transaction count -= completed bytes
```

例如，要用 TMA 把一个 `64 x 32` 的 `half` tile 从 Global Memory 搬到 Shared Memory：

```text
tile bytes = 64 * 32 * sizeof(half)
           = 64 * 32 * 2
           = 4096 bytes
```

启动复制前登记 4096 bytes：

```ptx
mbarrier.arrive.expect_tx.shared::cta.b64 _, [bar], 4096;
cp.async.bulk.shared::cta.global.mbarrier::complete_tx::bytes [smem_dst], [gmem_src], 4096, [bar];
```

第一条指令把 barrier 的 pending transaction count 增加 4096；它本身**不会启动数据复制**。第二条指令才启动异步复制，并把完成通知绑定到 `bar`。整项复制完成时，硬件对该 barrier 执行相应的 complete-tx 操作，以复制的数据字节数 4096 抵消先前登记的 4096。

事务字节数是 barrier 使用的逻辑记账量，不应把它当作可以观察逐字节复制进度的计数器。程序只通过 barrier 是否完成来判断已登记的异步操作是否全部完成。

例如连续两次各登记 4096 bytes：

```text
第一次 expect_tx: pending tx-count += 4096
第二次 expect_tx: pending tx-count += 4096
总计需要完成 8192 bytes
```

同一个 phase 可以登记多个异步操作；登记量会累加，完成量也会累减。程序必须保证登记的 expected bytes 与关联异步指令最终报告的 completed bytes 一致：登记过多会导致 barrier 一直无法完成，登记不足则可能让 barrier 过早完成。

arrival count 跟踪“还有多少参与者尚未 arrive”，transaction count 跟踪“还有多少异步数据量尚未完成”；两者都归零后，当前 phase 才能结束。

------

<br>

### 3.4 Phase 与 Phase Parity

`mbarrier` 可以重复使用。每完成一轮，barrier 自动进入下一 phase。常见的 parity 形式只观察 phase 的最低一位：

```text
Tile 0 -> parity 0
Tile 1 -> parity 1
Tile 2 -> parity 0
Tile 3 -> parity 1
```

这里同时存在两个状态，不能混为一谈：

- Shared Memory 中的 barrier phase：由硬件维护；
- 每个等待线程寄存器中的 `expectedParity`：由软件维护，表示该线程当前正在等待哪一轮完成。

例如：

```cpp
int expectedParity = 0;
for (...) {
    while (!try_wait(barrier, expectedParity)) {        /* 等待 expectedParity 所代表的这一轮完成。 */
        // 可以轮询，或在等待间隙执行独立工作
    }
    // 消费本轮数据
    // 只更新当前线程的本地期望值，为等待下一轮做准备。
    expectedParity ^= 1;
}
```

以最初等待 parity 0 为例，执行过程如下：

| 时刻 | Shared Memory 中的 barrier | 线程寄存器中的 `expectedParity` | 含义 |
|---|---|---:|---|
| 初始化后 | 正在执行 phase 0 | 0 | 线程准备等待 phase 0 完成 |
| phase 0 满足完成条件 | 硬件自动进入 phase 1 | 0 | `try_wait(..., 0)` 可以成功返回 |
| 执行 `expectedParity ^= 1` | 仍处于 phase 1 | 1 | 线程准备等待 phase 1 完成 |
| phase 1 满足完成条件 | 硬件自动进入 phase 2 | 1 | `try_wait(..., 1)` 可以成功返回 |
| 再次执行异或 | 仍处于 phase 2 | 0 | phase 2 的 parity 又是 0 |

因此，这里必须区分两个动作：

- barrier 在满足条件后由硬件**自动推进 phase**；
- 软件执行 `expectedParity ^= 1`，只是让该线程的本地期望值切换到下一轮。

真正的 phase 可以不断递增，而 parity 只保存它的最低一位，所以呈现为 `0 -> 1 -> 0 -> 1`。异或操作不会读取或修改 Shared Memory 中的 barrier 对象，更不是由软件用它来推进 barrier phase.

------

<br><br>

## 4. 初始化与登记异步事务

### 4.1 初始化

`mbarrier` 对象必须位于 Shared Memory，并满足 64-bit 对齐要求：

```ptx
mbarrier.init.shared::cta.b64 [bar], count;
```

- `bar`：Shared Memory 地址；
- `count`：expected arrival count。

常见作用域：

- `shared::cta`：同一 CTA 内使用；
- `shared::cluster`：Thread Block Cluster 内使用。

跨 CTA 访问 cluster shared address 时，需要按 PTX 规定映射地址。初始化错误、地址空间错误或未对齐都可能导致难以诊断的问题。

-----

<br>

### 4.2 `arrive.expect_tx`

```ptx
mbarrier.arrive.expect_tx.shared::cta.b64 _, [bar], tx_count;
```

它完成两件事：

1. 登记一次 arrival，减少 pending arrival count；
2. 把 `tx_count` 加入 pending transaction count。

随后发起与这个 barrier 关联的异步事务。异步事务完成时，硬件更新 transaction count。

-----

<br>

### 4.3 指令级 Global-to-Shared 搬运

下面把初始化、登记字节数、启动搬运和等待放在一起。代码省略了 Global/Shared 地址转换，但展示了四条关键 PTX 指令的关系：

**PTX：用 mbarrier 跟踪 Global → Shared bulk copy**

```ptx
// 约定：
//   p_leader  只有一个线程为 true
//   smem_dst  目标 Shared Memory 地址
//   gmem_src  源 Global Memory 地址
//   bytes     复制字节数，满足 cp.async.bulk 的对齐和大小要求
//   phase     当前软件维护的 parity

// 在 CTA Shared Memory 中分配一个 8-byte、8-byte 对齐的对象。
// 此时它只是一块存储空间，执行 mbarrier.init 后才成为有效的 mbarrier bar.
.shared .align 8 .b64 bar;

// 每个线程私有的 predicate（布尔）寄存器，用来接收 try_wait 的检查结果。
.reg .pred done;

// 1. 一个线程初始化 barrier；本例只有该线程贡献 arrival
@p_leader mbarrier.init.shared::cta.b64 [bar], 1;
barrier.cta.sync 0;

// 2. 登记一次 arrival，并增加待完成的事务字节数
@p_leader mbarrier.arrive.expect_tx.relaxed.cta.shared::cta.b64 _, [bar], bytes;

// 3. 启动 Global -> Shared 搬运；完成后硬件按 bytes 更新 bar
@p_leader cp.async.bulk.shared::cta.global.mbarrier::complete_tx::bytes [smem_dst], [gmem_src], bytes, [bar];

// 4. 消费者等待该 phase 完成，并取得 acquire 可见性
wait_for_tile:
    mbarrier.try_wait.parity.acquire.cta.shared::cta.b64 done, [bar], phase;
    @!done bra wait_for_tile;

// 从这里开始，消费者才读取 smem_dst
```

上例的完成链为：

```text
arrive 使 pending arrival count 归零
        +
TMA 完成后使 pending tx-count 归零
        ↓
barrier 自动推进 phase
        ↓
try_wait.parity.acquire 返回 true
```

!!! warning "不要重复计算 transaction count"
    `mbarrier.arrive.expect_tx` 登记的是 barrier 还要等待多少字节；使用 `.mbarrier::complete_tx::bytes` 的 `cp.async.bulk` 在完成时负责减少相应 transaction count。代码必须确保两边对同一事务使用一致的字节数。

------

<br>

### 4.4 使用 CUDA C++ Barrier API

如果不需要直接控制 PTX，可先用 `cuda::barrier` 与 `cuda::memcpy_async` 理解同一模型。下面是单阶段的 block-wide copy：

**CUDA C++：异步复制并用 barrier 等待**

```cpp
#include <cooperative_groups.h>
#include <cuda/barrier>

namespace cg = cooperative_groups;

template <int N>
__global__ void copy_then_compute(float* out, const float* in)
{
    cg::thread_block block = cg::this_thread_block();
    using barrier_t = cuda::barrier<cuda::thread_scope_block>;

    __shared__ barrier_t ready;
    __shared__ float tile[N];

    if (block.thread_rank() == 0) {
        init(&ready, block.size());
    }
    block.sync();

    // 全 CTA 协作发起 Global -> Shared 异步复制
    cuda::memcpy_async(
        block, tile, in, cuda::aligned_size_t<16>(N * sizeof(float)), ready );

    // 每个参与线程 arrive；最后还会等待绑定到本 phase 的 copy
    ready.arrive_and_wait();

    for (int i = threadIdx.x; i < N; i += blockDim.x) {
        out[i] = tile[i] * 2.0f;
    }
}
```

这个例子没有把 copy 和 compute 重叠，只展示 API 如何把异步 copy 绑定到 barrier。真正隐藏延迟需要多阶段缓冲区，见第 11 节。

-----

<br>

### 4.5 一个简化的 TMA 流程

```text
1. 在 Shared Memory 中初始化 mbarrier
2. producer 登记 expected transaction bytes
3. producer 启动 TMA
4. 执行与该数据无关的工作
5. consumer 等待当前 phase 完成
6. consumer 读取已经到达的 Shared Memory 数据
7. 下一轮复用 barrier 与缓冲区
```

伪代码：

```cpp
__shared__ uint64_t barrier;

if (elect_one_thread()) {
    mbarrier_init(&barrier, /* arrival count */ 1);
}
__syncthreads();

int phase = 0;

for (int tile = 0; tile < tile_count; ++tile) {
    if (elect_one_thread()) {
        mbarrier_arrive_expect_tx(&barrier, tile_bytes);
        launch_tma_load(/* gmem -> smem, completion -> barrier */);
    }

    while (!mbarrier_try_wait_acquire(&barrier, phase)) {
        // 可执行独立工作，或继续轮询
    }

    consume_tile_from_smem();
    phase ^= 1;
}
```

真实代码还需要正确处理 producer/consumer 协作、buffer stage、Proxy Fence、地址空间转换和架构限定。

-----

<br><br>

## 5. 等待 mbarrier

### 5.1 `test_wait` 与 `try_wait`

两者都检查目标 phase 是否完成，但调度语义不同：

| 指令 | 行为 | 适合场景 |
| --- | --- | --- |
| `mbarrier.test_wait` | 非阻塞测试一次，立即返回谓词 | 程序自行轮询，或每次检查间穿插其他工作 |
| `mbarrier.try_wait` | 尝试等待；允许使用 `suspendTimeHint` 暂时挂起线程 | 希望降低忙轮询压力 |

`try_wait` 不是“保证一直阻塞到完成”的传统 wait. 等待提示到期后 barrier 仍未完成时，它仍可返回 false。

实验结果显示：`test_wait` 和不带 hint 的 `try_wait` 约在 42 cycles 后得到结果；带 hint 的 `try_wait` 可能进入休眠路径，即使 barrier 已完成也可能约需 150 cycles。具体延迟依赖微架构与测试条件，不应当作 ISA 保证。

------

<br>

### 5.2 Parity 判断

等待指令接收当前线程所等待的 `phaseParity`：

```ptx
mbarrier.test_wait.parity.shared::cta.b64 waitComplete, [bar], phaseParity;
```

概念上：

- 输入 parity 与 barrier 当前 parity 相同：该 phase 仍在进行，返回 false；
- parity 已改变：之前等待的 phase 已完成，返回 true.

两种等待可以直接写成 PTX 循环。`test_wait` 适合在两次检查之间穿插工作：

**PTX：`test_wait` 忙轮询**

```ptx
poll:
    mbarrier.test_wait.parity.acquire.cta.shared::cta.b64 done, [bar], phase;
    @done bra tile_ready;

    // 可选：执行不依赖该 tile 的工作，或短暂 nanosleep
    nanosleep.u32 20;
    bra poll;

tile_ready:
    // 安全读取 Shared Memory
```

`try_wait` 可以附带 `timeHint`，允许硬件暂时挂起当前线程：

**PTX：`try_wait` 与 `suspendTimeHint`**

```ptx
try_again:
    mbarrier.try_wait.parity.acquire.cta.shared::cta.b64 done, [bar], phase, timeHint;
    @!done bra try_again;
```

如果程序希望轮询期间主动执行其他指令，应优先采用 `test_wait` 式控制流；如果只是在原地等待，可考虑 `try_wait`.

------

<br>

### 5.3 `.acquire`

“完成”不仅要告诉程序计数器已经归零，还要确保消费者随后读取的数据可见。带 `.acquire` 的等待在成功时建立 acquire 语义，使之后的内存访问不会越过等待提前发生。

这对以下场景尤其重要：

- TMA 写入 Shared Memory 后，线程从 Shared Memory 加载到寄存器；
- WGMMA 使用 Shared Memory 描述符与数据；
- 一个线程消费其他线程在 barrier 前发布的数据。

-----

<br><br>

## 6. 用 mbarrier 管理缓冲区复用

`mbarrier` 不只用于等待异步 copy. 它还可以保护普通 producer/consumer 流水线中的 Shared Memory.

考虑 WGMMA 正在读取 `sA`：

```text
Consumer/WGMMA: 读取 Tile K
Producer:       想把 Tile K+1 写入同一块 sA
```

若 producer 过早覆盖，就出现 WAR hazard。可以设置一个 barrier：

1. producer 等待 consumer 释放缓冲区；
2. consumer 完成最后一次读取后执行 arrive；
3. arrival count 归零，barrier 自动推进 phase；
4. producer 确认 phase 完成后写入下一 tile。

### 6.1 `mbarrier.arrive`

```ptx
mbarrier.arrive.shared::cta.b64 state, [bar], count;
```

它减少当前 phase 的 pending arrival count. 具体指令形式、`count` 操作数支持情况以及目标架构要求应以对应 PTX ISA 版本为准。

------

<br>

### 6.2 `.release`

consumer 在到达 barrier 前完成了对共享数据的访问或写入，可用 release 语义防止相关操作被重排到 arrive 之后：

```ptx
mbarrier.arrive.release.shared::cta.b64 state, [bar];
```

release 负责发布 arrive 之前的内存效果，等待侧的 acquire 负责约束后续消费。两者形成 producer/consumer 的有序交接。

-------

<br>

### 6.3 用 Empty/Full Barrier 防止覆盖

实际流水线常为每个 stage 准备两个状态：

- `full[stage]`：TMA 已经把新 tile 写入该 stage；
- `empty[stage]`：consumer 已经读完旧 tile，该 stage 可以覆盖。

**结构示例：每个 stage 的 Empty/Full 握手**

```cpp
for (int k = 0; k < tile_count; ++k) {
    int s = k % stages;

    wait_acquire(empty[s], empty_phase[s]);               /* Producer：等 Consumer 释放 stage 后才能覆盖 */
    arrive_expect_tx(full[s], tile_bytes);                /* 只登记 arrival 和预期事务字节，不启动复制 */
    tma_load(smem_tile[s], global_tile[k], full[s]);      /* 发起 TMA；complete-tx 向 full[s] 执行 release */

    wait_acquire(full[s], full_phase[s]);                 /* Consumer：等待 full，并取得 TMA 数据可见性 */
    wgmma_using(smem_tile[s]);                            /* 发出 WGMMA，不代表 Shared Memory 已读完 */
    wgmma_wait_until_reads_are_finished();                /* 确认 WGMMA 不再读取该 stage */

    mbarrier_arrive_release(empty[s]);                    /* Consumer 显式 release：通知 Producer 可复用 */

    full_phase[s]  ^= 1;
    empty_phase[s] ^= 1;
}
```

`wgmma_using()` 发出异步矩阵指令并不代表读取已结束；必须等到能证明该 stage 不再被 WGMMA 使用时，才能 arrive 到 `empty` barrier。否则 barrier 本身存在，但 arrive 的位置仍然错误。

------

<br>

### 6.4 编译器与硬件重排

单线程中互不依赖的操作可能被重排：

```cpp
a = 1;
b = 2;
c = a + b;
```

但在多线程代码中，另一个线程可能通过 `flag` 观察状态：

```cpp
// Producer
d[0][0] = 42.0f;
flag = 1;

// Consumer
while (flag == 0) {}
float x = d[0][0];
```

如果缺少正确的 release/acquire 关系，consumer 看到 `flag == 1` 并不自动保证同时看到新的 `d[0][0]`.

------

<br><br>

## 7. `arrive_drop`、`.noComplete` 与销毁

### 7.1 `mbarrier.arrive_drop`

`arrive_drop` 同时：

1. 减少当前 phase 的 pending arrival count；
2. 永久减少后续 phase 使用的 expected arrival count.

它适用于某个参与者完成当前轮后退出后续同步的情况。假设 expected arrival count 原为 8，本轮有两个参与者 drop，那么下一 phase 的 expected arrival count 将变为 6。

某些形式还可组合 `.expect_tx`、`.release` 或 `.relaxed`：

- `.expect_tx`：同时登记事务字节数；
- `.release`：发布 arrive/drop 之前的内存效果；
- `.relaxed`：不额外建立内存顺序。

-----

<br>

### 7.2 `.noComplete`

`.noComplete` 不是“禁止 barrier 完成”，而是程序向硬件作出的保证：

> 本次 arrive 只更新计数，绝不会成为使当前 phase 完成的最后一次 arrival.

例如初始 pending arrival count 为 8：

```text
arrive.noComplete count=3 -> 还剩 5，合法
arrive.noComplete count=8 -> 本应使 phase 完成，违反保证
```

如果带 `.noComplete` 的操作实际上满足了 phase completion 条件，行为未定义。真正可能完成 phase 的最后一次 arrival 应使用允许完成的普通形式。

**PTX：`.noComplete` 不是禁止完成**

```ptx
// 初始化 pending arrival count = 8
mbarrier.init.shared::cta.b64 [bar], 8;

// 合法：只减少 3，明确保证这次不会结束 phase
mbarrier.arrive.noComplete.release.cta.shared::cta.b64 state, [bar], 3;

// 此时还剩 5. 最后一次 arrival 必须使用允许完成的形式
mbarrier.arrive.release.cta.shared::cta.b64 _, [bar], 5;

// 错误示意：如果直接在初始状态执行 count=8，行为未定义
// mbarrier.arrive.noComplete... [bar], 8;
```

不同 PTX ISA 和目标架构对带显式 `count` 的 arrive 支持不同。例如 Ampere `sm_8x` 上某些带 `count` 的形式要求 `.noComplete`，Hopper `sm_90+` 才支持相应的可完成形式。编写内联 PTX 时必须核对版本表。

------

<br>

### 7.3 `mbarrier.inval`

当 barrier 不再使用时，可将其失效，使对应 Shared Memory 区域能够安全地用于其他对象：

```ptx
mbarrier.inval.shared::cta.b64 [bar];
```

它不会释放动态资源；本质是使该 64-bit barrier 对象失效。之后若要再次作为 barrier 使用，必须重新初始化。

------

<br><br>

## 8. Cluster Barrier

`bar.sync` / `__syncthreads()` 只能协调单个 Thread Block. H100 的 Thread Block Cluster 允许集群内不同 CTA 使用 Distributed Shared Memory，因此还需要 Cluster 范围的同步。

典型用途是 TMA Multicast：所有目标 CTA 必须已经驻留并完成必要初始化，发送者才能安全地向它们的 DSMEM 写入。

Cluster Barrier 采用 split-phase 形式：

```text
barrier.cluster.arrive  // 声明已到达，不必立即阻塞
执行与其他 CTA 无关的独立工作
barrier.cluster.wait    // 等到 cluster 中其他参与者也到达
```

更完整的 PTX 控制流如下：

**PTX：Cluster Barrier 的 arrive/wait 分离**

```ptx
st.shared::cluster.u32 [remote_addr], value;            // barrier 之前的普通内存访问由默认 release 语义发布
barrier.cluster.arrive.release.aligned;

mad.lo.s32 independent, a, b, c;                        // arrive 与 wait 之间只能执行不依赖其他 CTA 数据的工作

barrier.cluster.wait.acquire.aligned;
ld.shared::cluster.u32 result, [remote_addr];           // wait 返回后再读取其他 CTA 在 arrive 前发布的数据
```

若使用 `barrier.cluster.arrive.relaxed`，则 barrier 不再替你发布 arrive 之前的访问，程序需要根据依赖显式安排 fence。所有线程在一次 barrier 完成前只能 arrive 一次。

在 wait 成功返回后，程序才能依赖其他 CTA 在 barrier 前发布的数据。Cluster Barrier 的参与规则、收敛性和内存顺序要求必须按 CUDA/PTX 文档实现，不能把它当成任意线程均可随意调用的普通函数。

------

<br><br>

## 9. `cp.async.bulk` 的异步 Group

### 9.1 `bulk_group` 完成机制

某些 `cp.async.bulk` 形式使用 `.bulk_group` 完成机制。操作在发出时已经开始执行；随后通过 commit 和 wait 把这些操作按当前线程的 group 管理。

```ptx
cp.async.bulk.global.shared::cta.bulk_group [dst0], [src0], size0;
cp.async.bulk.global.shared::cta.bulk_group [dst1], [src1], size1;

cp.async.bulk.commit_group;
```

这类形式典型用于 Shared Memory 到 Global Memory 的异步写回。Global Memory 到 Shared Memory 的 TMA load 通常使用 `mbarrier::complete_tx::bytes` 完成机制，而不是这套 group 等待。

下面的例子连续提交两个 Shared-to-Global 写回 group，并利用 `.read` 尽早复用 Shared Memory：

**PTX：Shared → Global 写回流水线**

```ptx
// 第 0 批：可以包含多个 copy
cp.async.bulk.global.shared::cta.bulk_group [gmem_tile0_a], [smem_tile0_a], bytes_a;
cp.async.bulk.global.shared::cta.bulk_group [gmem_tile0_b], [smem_tile0_b], bytes_b;
cp.async.bulk.commit_group;

// 第 1 批
cp.async.bulk.global.shared::cta.bulk_group [gmem_tile1], [smem_tile1], bytes_1;
cp.async.bulk.commit_group;

// 允许最新 1 批继续 pending；第 0 批必须已完成源端读取
cp.async.bulk.wait_group.read 1;

// 现在可以覆盖 smem_tile0_a / smem_tile0_b，
// 但不能仅据此宣称对应 Global Memory 写入已经可见

// 最终需要完整完成时
cp.async.bulk.wait_group 0;
```

`cp.async.bulk` 的 `size` 必须满足对应指令的倍数与地址对齐要求。不要因为示例使用变量名 `bytes`，就假设任意字节数都合法。

------

<br>

### 9.2 `cp.async.bulk.commit_group`

`commit_group` 把当前线程此前发出、尚未提交的 `.bulk_group` 操作组成一个 group：

```text
发出 A
发出 B
commit_group    -> A、B 属于 Group 0

发出 C
commit_group    -> C 属于 Group 1
```

关键语义：

- 它不启动复制，复制在 `cp.async.bulk` 发出时已经开始；
- 它不等待复制完成；
- group 是 per-thread 的；
- 同一 group 内的多个操作没有彼此顺序保证；
- commit 之后发出的操作属于后续未提交集合。

因此，`commit_group` 可以理解为“给已经发出的异步操作画一个批次边界”。

-----

<br>

### 9.3 `cp.async.bulk.wait_group N`

```ptx
cp.async.bulk.wait_group N;
```

`N` 表示允许最新的多少个 committed group 仍处于 pending 状态。更老的 group 必须完成，指令才能继续。

假设依次提交 Group 0、1、2，其中 Group 2 最新：

| 指令 | 返回时的保证 |
| --- | --- |
| `wait_group 0` | Group 0、1、2 全部完成 |
| `wait_group 1` | Group 0、1 完成，允许 Group 2 未完成 |
| `wait_group 2` | Group 0 完成，允许 Group 1、2 未完成 |
| `wait_group 3` | 不要求等待这三个 group |

所以 `N` 不是“等待第 N 组”，也不是“等待 N 组完成”，而是“最多保留 N 个最新未完成组”。

双缓冲中的直观用法：

```ptx
// 提交 tile 0
cp.async.bulk.commit_group;

// 提交 tile 1
cp.async.bulk.commit_group;

// 确保较老的 tile 0 已完成，允许 tile 1 继续执行
cp.async.bulk.wait_group 1;
```

-----

<br>

### 9.4 `cp.async.bulk.wait_group.read N`

`.read` 形式只等待异步操作完成对**源数据的读取**，不要求目的端写入已经完全结束。

典型方向：

```text
Shared Memory ──TMA──> Global Memory
```

执行：

```ptx
cp.async.bulk.wait_group.read 0;
```

可以保证：

- TMA 已经读完相关 Shared Memory；
- 程序可以覆盖或复用源缓冲区。

但不能据此保证：

- 数据已经完整写入 Global Memory；
- 其他观察者已经能看到 Global Memory 中的新值。

对比：

```text
wait_group.read 0  -> 源端已读完，可以提前释放源缓冲区
wait_group 0       -> 对应 group 的完整异步操作已经完成
```

这一区别能缩短 Shared Memory buffer 的占用时间，是流水化写回的重要优化点。

------

<br><br>

## 10. Named Barrier

`__syncthreads()` 是整个 CTA 的 rendezvous：所有参与线程都要到达，才能继续执行。Named Barrier 则允许在一个 CTA 内建立多个独立同步点，不同 Warp 子集可以分别协作，而不必停止无关 Warp。

示意语法：

```ptx
barrier.cta.sync.aligned   barrier_id, participating_threads;
barrier.cta.arrive.aligned barrier_id, participating_threads;
```

- `barrier_id`：Named Barrier 编号；
- `participating_threads`：参与线程数。

不同 Warp 可以对同一个 Named Barrier 使用不同操作，例如 producer 使用 arrive，consumer 使用 sync，从而构造 CTA 内的 producer/consumer 流水线。

下面展示两个 Warp 的单轮交接。`64` 表示两个 Warp 共 64 个参与线程：

**CUDA C++ + inline PTX：两个 Warp 的 Named Barrier**

```cpp
__device__ __forceinline__ void named_arrive_64(){
    asm volatile("barrier.cta.arrive.aligned 3, 64;" ::: "memory");
}

__device__ __forceinline__ void named_sync_64(){
    asm volatile("barrier.cta.sync.aligned 3, 64;" ::: "memory");
}

__global__ void producer_consumer(float* out){
    __shared__ float tile[32];
    int warp = threadIdx.x / 32;
    int lane = threadIdx.x % 32;

    if (warp == 0) {
        tile[lane] = produce(lane);
        named_arrive_64();      // Warp 0 到达后继续，不等待 Warp 1
        do_independent_work();
    } else if (warp == 1) {
        named_sync_64();        // Warp 1 到达并等待总计 64 个线程
        out[lane] = consume(tile[lane]);
    }
}
```

参与线程数必须符合 PTX 的限制，且同一 Warp 中执行 `.aligned` 形式的线程必须收敛。循环复用时还要避免 producer 在 barrier 重置前重复 arrive；工程实现常用两个 barrier ID 做 ping-pong。

Named Barrier 与 `mbarrier` 不应混为一谈：

| 机制 | 主要用途 |
| --- | --- |
| `bar.sync` / `__syncthreads()` | 整个 CTA 的线程会合 |
| Named Barrier | CTA 内部分线程或 Warp 子集的同步 |
| `mbarrier` | Shared Memory 中的可复用 barrier，可跟踪 arrival 与异步事务 |
| Cluster Barrier | 同一 Thread Block Cluster 内的 CTA 协作 |
| Async Group | 当前线程按组管理某些 `cp.async.bulk` 操作 |


----

<br><br>

## 11. 把这些机制放进同一条流水线

下面以双缓冲为例串联主要机制：

```text
Producer                          Consumer
--------                          --------
等待 buffer[0] 可复用
登记 Tile 0 的事务字节数
启动 TMA: Global -> buffer[0]

登记 Tile 1 的事务字节数            等待 Tile 0 的 mbarrier/acquire
启动 TMA: Global -> buffer[1]     使用 Tile 0 计算

等待 buffer[0] 可复用              等待 Tile 1 的 mbarrier/acquire
覆盖 buffer[0] 为 Tile 2          使用 Tile 1 计算
...
```

这里至少包含三种不同的依赖：

1. **数据到达依赖**：消费者通过 `mbarrier` 确认 TMA 已把 tile 写入 Shared Memory；
2. **内存可见性依赖**：消费侧通过 acquire 获得可见性，生产侧需要提供与之匹配的 release；若数据跨不同 Proxy 交接，该 release 还必须通过相应的 Proxy Fence 或异步完成机制建立。这里 release 是内存顺序语义，Proxy Fence 是跨 Proxy 建立顺序的机制，两者并非互斥关系；
3. **缓冲区复用依赖**：生产者必须确认消费者或异步引擎已经读完旧 tile，才能覆盖 buffer。

同一个 barrier 或 wait 并不会自动解决所有三类问题。设计流水线时，应分别回答：

- 谁生产数据，谁消费数据？
- 数据通过哪个 Proxy 读写？
- 完成条件是线程到达、事务字节归零，还是 group 完成？
- 等待的是“源端已经读完”，还是“完整操作已经完成”？
- 谁负责维护下一轮的 phase parity？

### 11.1 多阶段预取代码骨架

下面用 CUDA C++ TMA API 展示多阶段结构。代码保留了真正决定流水线正确性的顺序，但把边界处理和计算函数省略了：

**CUDA C++ 结构示例：多阶段 TMA 预取**

```cpp
#include <cooperative_groups.h>
#include <cuda/barrier>
#include <cuda/ptx>

namespace cg  = cooperative_groups;
namespace ptx = cuda::ptx;

template <int BLOCK_SIZE, int STAGES>
__global__ void staged_kernel(int* out, const int* in, int tile_count)
{
    using barrier_t = cuda::barrier<cuda::thread_scope_block>;

    __shared__ barrier_t ready[STAGES];
    __shared__ int smem[STAGES][BLOCK_SIZE];

    cg::thread_block block = cg::this_thread_block();
    bool leader = (block.thread_rank() == 0);

    if (leader) {
        for (int s = 0; s < STAGES; ++s) {
            init(&ready[s], 1);                                         /* 一个 elected producer 贡献 arrival */
        }
    }
    block.sync();

    int phase[STAGES] = {};

    // Prologue：先填满流水线
    if (leader) {
        for (int s = 0; s < STAGES && s < tile_count; ++s) {
            size_t bytes = BLOCK_SIZE * sizeof(int);
            cuda::device::memcpy_async_tx(
                smem[s], in + s * BLOCK_SIZE, cuda::aligned_size_t<16>(bytes), ready[s] );
            (void)cuda::device::barrier_arrive_tx(ready[s], 1, bytes);
        }
    }

    for (int tile = 0; tile < tile_count; ++tile) {
        int s = tile % STAGES;

        // 等当前 stage 的数据到达；真实实现可用官方 PTX wrapper
        while (!ptx::mbarrier_try_wait_parity(
            ptx::sem_acquire, ptx::scope_cta, cuda::device::barrier_native_handle(ready[s]), phase[s])) { }

        for (int i = threadIdx.x; i < BLOCK_SIZE; i += blockDim.x) {
            out[tile * BLOCK_SIZE + i] = compute(smem[s][i]);
        }
        block.sync();                                       // 所有消费者读完后，才能覆盖 stage s

        int next = tile + STAGES;
        if (leader && next < tile_count) {
            size_t bytes = BLOCK_SIZE * sizeof(int);
            cuda::device::memcpy_async_tx(
                smem[s], in + next * BLOCK_SIZE, cuda::aligned_size_t<16>(bytes), ready[s] );
            (void)cuda::device::barrier_arrive_tx(ready[s], 1, bytes);
        }

        phase[s] ^= 1;
    }
}
```

这段代码揭示了多阶段流水线的四个固定部分：

1. prologue 预填多个 stage；
2. 等待当前 stage；
3. 计算当前 tile；
4. 把距离当前 tile 恰好 `STAGES` 的后续 tile 预取到刚释放的 stage.

本段代码的 producer 只是一个 thread，注意到该 thread 实际上既参与数据预取，也参与计算任务，但是由于数据预取是异步完成的，所以该线程并未成为 bottleneck. 工程代码还应处理 tile 尾部、TMA 对齐、Tensor Map 生命周期、Warp 专职分工，以及“消费者何时真正读完”的精确条件。

## 12. 常见误区

### 误区一：发出异步指令后，下一条指令自然能看到结果

异步指令只表示工作已经提交。消费者必须使用对应的完成与内存顺序机制。

### 误区二：`__syncthreads()` 可以替代 Proxy Fence

`__syncthreads()` 负责 CTA 内线程协调；Proxy Fence 负责不同访问机制之间的顺序。跨 Proxy 且由多个线程共同生产数据时，两者可能都需要。

### 误区三：phase 由软件手动翻转

barrier 满足完成条件后自动进入下一 phase。软件异或的是线程自己保存的等待 parity。

### 误区四：`wait_group 2` 表示等待两个 group

它表示允许最新两个 group 仍然 pending；更老的 group 必须已经完成。

### 误区五：`wait_group.read` 等价于完整完成

它只确认源数据已经被异步引擎读完，但是写入是否完成是未知的，主要用于提前复用源缓冲区。

### 误区六：`.noComplete` 会强制 barrier 不完成

它是程序作出的前提保证。如果该 arrive 实际会完成 phase，则行为未定义。

### 误区七：所有 TMA 都使用 `commit_group`

`mbarrier` 和 `bulk_group` 是两套独立的异步完成跟踪机制，具体使用哪一套取决于 `cp.async.bulk` 的指令形式：

| 对比项 | `mbarrier` | `bulk_group` |
|---|---|---|
| 状态存放 | 显式的 Shared Memory barrier 对象 | 发起线程隐式维护的 bulk async-group |
| 完成记账 | arrival count、transaction count 和 phase | 将操作提交成 group，并跟踪 pending group |
| 提交/登记 | `mbarrier.arrive.expect_tx` 等 | `cp.async.bulk.commit_group` |
| 等待方式 | `mbarrier.test_wait` / `mbarrier.try_wait` | `cp.async.bulk.wait_group` / `wait_group.read` |
| 典型用途 | 数据到达 Shared Memory 后供 CTA/Cluster 协作消费 | 发起线程跟踪 Shared-to-Global 写回或源缓冲区读取完成 |

本文涉及的常见方向如下：

| 数据方向 | 完成机制 |
|---|---|
| Global → CTA Shared | `mbarrier` |
| Global → Cluster Shared | `mbarrier` |
| CTA Shared → Cluster Shared | `mbarrier` |
| CTA Shared → Global | `bulk_group` |

两者不能交叉等待：使用 `.mbarrier::complete_tx::bytes` 发起的操作应通过对应的 `mbarrier` 等待；使用 `.bulk_group` 发起的操作应通过 `cp.async.bulk.commit_group` 和 `cp.async.bulk.wait_group` 跟踪。`.bulk_group` 操作没有绑定到某个 Shared Memory barrier，因此 `mbarrier.try_wait` 不会替它报告完成。

mbarrier 是非线程的，通过 shared memory 的某一个地址来同步一个 block 或者 cluster 等线程组织单元。bulk_group 是线程级的，每一个 thread 需要自己维护两个队列：未提交的 group、已提交的 group. 某一个 thread 不能依据其他线程的 group 提交情况来判断 shared memory 是否空出，类似于“个人自扫门前雪，莫管他人瓦上霜”。

----

<br><br>

## 13. 参考资料

- [PTX ISA：Proxies](https://docs.nvidia.com/cuda/parallel-thread-execution/#proxies)
- [PTX ISA：mbarrier instructions](https://docs.nvidia.com/cuda/parallel-thread-execution/#parallel-synchronization-and-communication-instructions-mbarrier)
- [PTX ISA：cp.async.bulk](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cp-async-bulk)
- [PTX ISA：cp.async.bulk.commit_group](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cp-async-bulk-commit-group)
- [PTX ISA：cp.async.bulk.wait_group](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cp-async-bulk-wait-group)
- [CUDA Programming Guide：Asynchronous Data Copies](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/async-copies.html)
