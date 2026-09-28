# 欢迎

这里是 dongjian 的个人笔记站。

<div class="grid cards" markdown>

-   :material-laptop:{ .lg .middle } __Computer Science__

    ---

    编程语言 & ISA、计算机系统、算法、HPC、理论计算机

    [:octicons-arrow-right-24: 进入](cs/index.md)

-   :material-chip:{ .lg .middle } __NVIDIA H100__

    ---

    GPU 异步执行、TMA、屏障与底层编程

    [:octicons-arrow-right-24: 进入](cs/h100/index.md)

</div>

## 关于本站

用 [MkDocs](https://www.mkdocs.org/) + [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/) 构建，
源码在 [Griffindorr/profile](https://github.com/Griffindorr/profile)，
每次 push 后由 GitHub Actions 自动构建并发布到 GitHub Pages。

??? note "新增一个板块时，复制这段模板"

    在 `mkdocs.yml` 的 `nav` 里加好新页面后，把下面这块粘到上方 `</div>` 之前：

    ```markdown
    -   :material-folder-outline:{ .lg .middle } __板块名__

        ---

        一句话说明这个板块放什么

        [:octicons-arrow-right-24: 进入](路径/index.md)
    ```

    图标从 Material 自带的 [图标库][icons] 里挑，把 `material-folder-outline`
    换成对应名字即可，不需要额外下载任何文件。

[icons]: https://squidfunk.github.io/mkdocs-material/reference/icons-emojis/
