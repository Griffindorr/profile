嵌入式处理器

1. 嵌入式微控制器（MCU，Micro Control Unit），即单片机，性能很差。
2. 嵌入式 DSP 处理器（DSP，Digital Signal Processor），太专用，可以用作一个外设；
3. 嵌入式微处理器（MPU，Micro Processing Unit）
4. 嵌入式片上系统（System on Chip）

ARM $\rightarrow$ Advanced RISC Machine

MIPS $\rightarrow$ Micro-Processor without Interlocked Pipeline Stages，无互锁流水级微处理器

ARM 结构在嵌入式系统市场中处于绝对主导，因为其功耗低、成本低、功能强，在嵌入式场景下由于 X86 架构。

Cortex IP 核是嵌入式处理器里最核心的 CPU 部分设计，但嵌入式处理器本身是芯片厂商基于这个核再集成外设和存储后做出来的完整芯片。

另外，Cortex 不只用于嵌入式：

- Cortex-M：典型嵌入式 MCU
- Cortex-R：实时嵌入式，如汽车气囊芯片；
- Cortex-A：典型嵌入式 MPU，应用处理器，手机、Linux 设备
- Cortex-X：高性能大核，旗舰手机

处理器三大领域：服务器、桌面 PC、嵌入式。

ARM 

1. 用户模式（User）：程序正常执行的模式；
2. 快终端模式（FIQ）：用于高速数据传输和请求通道；