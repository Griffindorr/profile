# 汇编分析实验代码

本目录保存《高级语言特性在汇编层面的映射》使用的 C++17 探针源码。实验通过 [Compiler Explorer](https://godbolt.org/) 在线编译源码，观察不同编译器和 ABI 生成的汇编；文档中的汇编片段是编译产物。故当前文件夹下的代码不以运行程序为目标，因此不提供本地 Release 可执行文件，也没有本地运行命令。

<br><br>

## 目录结构

```text
code/
├── abi_probe.cpp           参数传递、结构体返回、可变参数及 x86 调用约定
├── stack_frame_probe.cpp   栈帧、栈参数、隐藏返回地址、red zone
├── object_probe.cpp        结构体布局、成员函数、对象构造和虚调用
├── optimization_probe.cpp  优化等级、函数内联与循环向量化
└── README.md               实验环境与使用说明
```

<br><br>

## 开发与使用环境

打开 [Compiler Explorer](https://godbolt.org/), 每次将一个 `.cpp` 文件的完整内容复制到编辑器中，选择对应编译器，并在汇编窗口查看输出。各文件应分别编译，不要合并为一个程序。

| 编译器目标 | 基本编译选项 | 对应 ABI |
| :--- | :--- | :--- |
| x86-64 GCC 16.2 | `-O0 -std=c++17 -masm=intel` | System V AMD64 ABI |
| x64 MSVC v19.latest | `/Od /std:c++17` | Microsoft x64 ABI |
| x86 MSVC v19.latest | `/Od /std:c++17` | Microsoft x86 ABI |

- 分析 `optimization_probe.cpp` 时，将 GCC 的 -O0 分别改为 -O1, -O2, -O3 比较结果。
- 分析 red zone 时，按正文说明对 `stack_frame_probe.cpp` 比较 `-O0 -fno-stack-protector` 和额外添加 `-mno-red-zone` 的结果。

编译器版本、目标架构、选项和汇编显示过滤器可能影响具体输出，复现实验时请与正文所述配置保持一致。目录中只保留源码和本说明，不包含目标文件或其他编译中间产物。
