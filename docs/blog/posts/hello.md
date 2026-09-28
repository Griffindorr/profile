---
date: 2026-09-28
categories:
  - 公告
tags:
  - 站点
---

# 站点开张

这是这个笔记本的第一篇文章，用来演示博客板块的样子。

博客和笔记的区别很简单：

- **笔记**（`docs/` 下其他目录）是**长期维护**的知识，会反复修订
- **博客**（`docs/blog/posts/`）是**带日期**的记录，写下就不再改

## 怎么写一篇

在 `docs/blog/posts/` 下新建 `.md` 文件，头部写好日期和分类：

```yaml
---
date: 2026-09-28
categories:
  - 公告
tags:
  - 站点
---
```

插件会自动把它排进 [博客首页](../index.md)，并按 `categories` 和 `tags` 生成归档。

!!! tip "不想要博客板块？"
    删掉 `docs/blog/` 整个目录，并从 `mkdocs.yml` 里移除 `material/blog` 插件和 `nav` 里对应的条目即可。
