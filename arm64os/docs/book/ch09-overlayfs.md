# Phase 9：overlayfs 三层联合挂载

## 知识来源总览

- **overlayfs 设计规范**：约 35%（层次合并、copy-up、whiteout）
- **容器存储模型**：约 25%（镜像共享、运行时隔离）
- **VFS 框架**：约 20%
- **POSIX 文件语义**：约 10%（rename 原子性）
- **前序 Phase 依赖**：约 10%

## 为什么需要 overlayfs

Docker 容器镜像约 78MB。1000 个容器如果每个复制一份需要 78GB。解决：共享只读层（lower），每个容器只存差异（upper）。

```
upper (XFS, 读写)：容器运行时修改
work  (XFS)：      copy-up 临时目录
lower (squashfs)： 容器镜像（只读，所有容器共享）
─────────────────
merged：           用户看到的统一视图
```

## 核心操作

### ovl_lookup —— 层次查找

```c
struct dentry *ovl_lookup(struct inode *dir, struct dentry *dentry,
                          unsigned int flags) {
    /* 1. 先在 upper 查找 */
    upper = lookup_one_len(name, upper_dir, namelen);
    if (upper && upper->d_inode) {
        if (ovl_is_whiteout(upper))
            return d_splice_alias(NULL, dentry);  /* 被删除 */
        exists_upper = true;
    }

    /* 2. upper 没有，依次查 lower 层 */
    if (!exists_upper) {
        for (i = 0; i < num_lower; i++) {
            lower = lookup_one_len(name, lower_dir[i], namelen);
            if (lower && lower->d_inode) {
                exists_lower = true;
                break;  /* 高优先级 lower 命中，停止 */
            }
        }
    }

    /* 3. 记录文件来源层 */
    oe->has_upper = exists_upper;
    return d_splice_alias(new_inode, dentry);
}
```

**upper 优先**：upper 存在意味着文件已被修改或新建，应该用 upper 版本。

### copy-up —— 写时复制

```c
int ovl_copy_up_one(struct dentry *parent, struct dentry *dentry, int flags) {
    /* 1. 获取 lower 文件信息 */
    vfs_getattr(&lowerpath, &stat, STATX_ALL, 0);

    /* 2. 在 work 目录创建临时文件（原子性保证）*/
    tmp = ovl_create_temp(workdir, &stat);

    /* 3. 复制文件内容 */
    ovl_copy_file_range(lower_file, upper_file, stat.size);

    /* 4. 复制元数据 */
    ovl_copy_up_inode(dentry, tmp, &stat);

    /* 5. 原子 rename: work/tmp → upper/filename */
    ovl_do_rename(workdir, tmp, upperdir, upper_child, 0);

    /* 6. 更新缓存 */
    ovl_inode_update(dentry->d_inode, upper_child);
}
```

**work 目录的作用**：如果直接在 upper 创建，复制到一半断电会留下半成品。work 目录 + rename 的组合保证原子性——POSIX 要求 rename 是原子操作。

### whiteout —— 删除标记

```c
#define WHITEOUT_MODE   (S_IFCHR | 0000)
#define WHITEOUT_MAJOR  0
#define WHITEOUT_MINOR  0
```

删除 lower 层文件时，不能修改只读 lower，而是在 upper 创建同名的特殊字符设备文件 (0,0)。查找时遇到 whiteout → 视为文件不存在。

为什么用字符设备 (0,0)？不可能自然出现、不占磁盘空间、内核快速识别。

### 目录合并 (readdir)

`ls /merged/` 需要合并 upper + lower 的目录项：
1. 先遍历 upper（优先级高）
2. 再遍历 lower，跳过与 upper 同名的条目
3. 跳过被 whiteout 遮挡的 lower 条目
