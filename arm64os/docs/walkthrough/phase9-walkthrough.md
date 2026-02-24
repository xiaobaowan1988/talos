# Phase 9 Walkthrough: overlayfs 三层联合挂载

> **目标**：实现 Docker 容器存储的核心机制 — 联合文件系统。
> **最终效果**：读穿（read-through）、写时复制（copy-up）、文件删除（whiteout）全部工作。

---

## 9.1 容器存储问题

Docker 容器需要：

1. **共享基础镜像** — 100 个容器用同一个 Ubuntu 镜像，不能复制 100 份
2. **每个容器有独立修改** — 容器 A 装了 nginx，不影响容器 B
3. **删除文件也不能影响镜像** — 容器删了 `/etc/hostname`，镜像里还在

解决方案：**overlayfs** — 将只读镜像层和可写层叠加成统一视图。

---

## 9.2 overlayfs 三层架构

```
  ┌─────────────────────────────────────────┐
  │              merged (合并视图)            │  ← 用户看到的统一目录
  │         /merged                          │
  └────────────┬────────────────────────────┘
               │ 联合挂载
  ┌────────────┴───┬────────────────────────┐
  │   upper (上层)  │    lower (下层)         │
  │   /upper        │    /sq (squashfs)       │
  │   可读写 (ramfs) │    只读                 │
  │                 │                         │
  │  修改/新增的文件 │    基础镜像文件          │
  └────────────────┴─────────────────────────┘

  + work 目录 (/work): 内部使用，copy-up 的临时暂存区
```

### 挂载命令

```c
/* 创建必要目录 */
mkdir("/upper");
mkdir("/work");

/* 挂载 overlayfs */
do_mount("overlay", "/merged", "overlay", 0,
         "lowerdir=/sq,upperdir=/upper,workdir=/work");
```

---

## 9.3 核心数据结构

```c
/* overlayfs 超级块私有数据 */
struct ovl_fs {
    struct dentry *lower_root;  /* 只读层根 dentry (/sq) */
    struct dentry *upper_root;  /* 可写层根 dentry (/upper) */
    struct dentry *work_root;   /* 工作目录 dentry (/work) */
};

/* overlayfs inode 私有数据 */
struct ovl_inode_info {
    struct dentry *upper_dentry;  /* 上层对应的 dentry（可能为空） */
    struct dentry *lower_dentry;  /* 下层对应的 dentry（可能为空） */
};
```

---

## 9.4 路径查找 — `ovl_lookup()`

当打开 `/merged/hello.txt` 时，overlayfs 的 lookup 逻辑：

```c
static struct dentry *ovl_lookup(struct inode *dir,
                                 struct dentry *dentry, unsigned int flags)
{
    struct ovl_fs *ofs = dir->i_sb->s_fs_info;
    struct ovl_inode_info *info;
    struct dentry *upper = NULL, *lower = NULL;

    /* Step 1: 在上层（/upper）查找 */
    upper = lookup_in_layer(ofs->upper_root, dentry->d_name.name);

    if (upper && upper->d_inode) {
        /* Step 2: 检查是否是 whiteout（删除标记） */
        if (ovl_is_whiteout(upper->d_inode)) {
            /* 文件被删除了，返回 "不存在" */
            return NULL;  /* negative dentry */
        }
        /* 上层有文件 → 使用上层版本 */
    }

    if (!upper || !upper->d_inode) {
        /* Step 3: 上层没有，在下层（/sq）查找 */
        lower = lookup_in_layer(ofs->lower_root, dentry->d_name.name);
    }

    if (!upper && !lower)
        return NULL;  /* 两层都没有 */

    /* Step 4: 创建 overlay inode */
    struct inode *ovl_inode = new_inode(dir->i_sb);
    info = alloc_ovl_inode_info();
    info->upper_dentry = upper;
    info->lower_dentry = lower;
    ovl_inode->i_private = info;

    /* 用实际文件的属性 */
    struct inode *real = upper ? upper->d_inode : lower->d_inode;
    ovl_inode->i_mode = real->i_mode;
    ovl_inode->i_size = real->i_size;
    ovl_inode->i_op = &ovl_file_inode_ops;
    ovl_inode->i_fop = &ovl_file_ops;

    d_add(dentry, ovl_inode);
    return dentry;
}
```

```
  查找 /merged/hello.txt:

  ┌──────────────────────────────────┐
  │  1. 检查 upper (/upper/hello.txt)│
  │     → 不存在                     │
  ├──────────────────────────────────┤
  │  2. 检查 lower (/sq/hello.txt)   │
  │     → 存在！                     │
  ├──────────────────────────────────┤
  │  3. 创建 overlay inode           │
  │     lower_dentry = /sq/hello.txt │
  │     upper_dentry = NULL          │
  └──────────────────────────────────┘
```

---

## 9.5 读取 — 透明读穿

```c
static ssize_t ovl_read(struct file *filp, char *buf,
                        size_t count, loff_t *pos)
{
    struct ovl_inode_info *info = filp->f_inode->i_private;

    /* 优先使用上层，回退到下层 */
    struct dentry *real_dentry = info->upper_dentry
                               ? info->upper_dentry
                               : info->lower_dentry;

    /* 构造临时 file 对象指向真实 inode */
    struct file real_file;
    real_file.f_inode = real_dentry->d_inode;
    real_file.f_op = real_dentry->d_inode->i_fop;
    real_file.f_pos = *pos;

    /* 调用真实文件系统的 read */
    ssize_t ret = real_file.f_op->read(&real_file, buf, count, pos);

    return ret;
}
```

**效果**：读 `/merged/hello.txt` → 直接从 squashfs 读取，不产生任何复制。

---

## 9.6 写入 — 触发 copy-up

当写入一个只在下层存在的文件时，必须先把它复制到上层：

```c
static ssize_t ovl_write(struct file *filp, const char *buf,
                         size_t count, loff_t *pos)
{
    struct ovl_inode_info *info = filp->f_inode->i_private;

    /* 如果只有下层，需要 copy-up */
    if (!info->upper_dentry && info->lower_dentry) {
        ovl_copy_up_one(filp->f_inode);
    }

    /* 现在上层一定有文件，写入上层 */
    struct file real_file;
    real_file.f_inode = info->upper_dentry->d_inode;
    real_file.f_op = info->upper_dentry->d_inode->i_fop;
    real_file.f_pos = *pos;

    ssize_t ret = real_file.f_op->write(&real_file, buf, count, pos);

    /* 更新 overlay inode 大小 */
    filp->f_inode->i_size = info->upper_dentry->d_inode->i_size;

    return ret;
}
```

### copy-up 过程

```c
int ovl_copy_up_one(struct inode *ovl_inode)
{
    struct ovl_inode_info *info = ovl_inode->i_private;
    struct dentry *lower = info->lower_dentry;

    /* Step 1: 从下层读取全部数据 */
    char tmp_buf[4096];
    struct file lower_file;
    lower_file.f_inode = lower->d_inode;
    lower_file.f_op = lower->d_inode->i_fop;
    loff_t pos = 0;
    ssize_t size = lower_file.f_op->read(&lower_file, tmp_buf,
                                          sizeof(tmp_buf), &pos);

    /* Step 2: 在上层创建同名文件 */
    struct dentry *upper_parent = ovl_fs->upper_root;
    struct dentry *upper_dentry = d_alloc(upper_parent, &lower->d_name);
    upper_parent->d_inode->i_op->create(upper_parent->d_inode,
                                         upper_dentry,
                                         lower->d_inode->i_mode);

    /* Step 3: 写入数据到上层 */
    struct file upper_file;
    upper_file.f_inode = upper_dentry->d_inode;
    upper_file.f_op = upper_dentry->d_inode->i_fop;
    pos = 0;
    upper_file.f_op->write(&upper_file, tmp_buf, size, &pos);

    /* Step 4: 更新 overlay inode 信息 */
    info->upper_dentry = upper_dentry;

    return 0;
}
```

```
  写入 /merged/hello.txt (原来只在下层):

  Before:
  upper: (空)
  lower: hello.txt ("Hello from squashfs!")

  Copy-up:
  1. 读取 lower/hello.txt → 内存
  2. 创建 upper/hello.txt
  3. 写入内存内容到 upper/hello.txt
  4. 更新 upper_dentry

  After:
  upper: hello.txt ("Hello from squashfs!" + 新数据)
  lower: hello.txt ("Hello from squashfs!") ← 未改变！

  后续读写都走 upper
```

---

## 9.7 删除 — whiteout 标记

删除一个来自下层的文件不能真的删除（下层是只读的）。解决方案：在上层创建一个 **whiteout** 文件。

```c
/* whiteout = 字符设备 (0,0) */
static int ovl_do_whiteout(struct dentry *upper_parent, const char *name)
{
    /* 在上层创建一个文件 */
    struct qstr qname = { .name = name, .len = strlen(name) };
    struct dentry *wh = d_alloc(upper_parent, &qname);
    upper_parent->d_inode->i_op->create(upper_parent->d_inode, wh, 0);

    /* 修改为字符设备类型 */
    wh->d_inode->i_mode = S_IFCHR;  /* 字符设备 */
    wh->d_inode->i_fop = NULL;       /* 不可读写 */

    return 0;
}

/* 检查是否是 whiteout */
static int ovl_is_whiteout(struct inode *inode)
{
    return S_ISCHR(inode->i_mode);  /* 字符设备 = whiteout */
}
```

### 删除流程

```c
static int ovl_unlink(struct inode *dir, struct dentry *dentry)
{
    struct ovl_inode_info *info = dentry->d_inode->i_private;

    /* 如果上层有文件，删除上层文件 */
    if (info->upper_dentry) {
        /* 实际删除上层 dentry */
    }

    /* 如果下层有文件，创建 whiteout */
    if (info->lower_dentry) {
        ovl_do_whiteout(ovl_fs->upper_root, dentry->d_name.name);
    }

    return 0;
}
```

```
  删除 /merged/readme.txt (只在下层存在):

  Before:
  upper: (空)
  lower: readme.txt

  After:
  upper: readme.txt [WHITEOUT: S_IFCHR]  ← 遮挡下层
  lower: readme.txt                       ← 未改变

  后续 lookup:
  1. 查上层 → 找到 readme.txt
  2. 检测到 whiteout → 返回 "不存在"
  3. 不会查下层
```

---

## 9.8 测试验证

```c
static void test_phase9(void)
{
    /* Test 1: 读穿 — 读取只在下层的文件 */
    int fd = do_sys_open(&init_files, "/merged/hello.txt", O_RDONLY, 0);
    char buf[64];
    vfs_read(fget(&init_files, fd), buf, 64);
    /* 内容 == "Hello from squashfs!" — 直接从下层读取 */

    /* Test 2: 写入 — 触发 copy-up */
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_WRONLY, 0);
    vfs_write(fget(&init_files, fd), "Modified!\n", 10);
    /* copy-up: 下层→上层复制 → 然后写入 */

    /* 验证上层有了文件 */
    fd = do_sys_open(&init_files, "/upper/hello.txt", O_RDONLY, 0);
    /* 成功！copy-up 创建了上层副本 */

    /* Test 3: 删除 — whiteout */
    do_sys_unlink(&init_files, "/merged/readme.txt");

    /* 验证文件不可见 */
    fd = do_sys_open(&init_files, "/merged/readme.txt", O_RDONLY, 0);
    /* fd < 0 — 文件被 whiteout 遮挡 */

    /* 验证下层文件未被修改 */
    fd = do_sys_open(&init_files, "/sq/readme.txt", O_RDONLY, 0);
    /* 成功！下层完好无损 */
}
```

---

## 9.9 Phase 9 核心概念总结

| 概念 | 说明 |
|------|------|
| **lower** | 只读层（squashfs 镜像），共享不可修改 |
| **upper** | 可写层（ramfs），每个容器独立 |
| **work** | 工作目录，copy-up 暂存区 |
| **merged** | 联合视图，用户看到的统一目录 |
| **read-through** | 上层没有就从下层读取 |
| **copy-up** | 写入下层文件时先复制到上层 |
| **whiteout** | S_IFCHR(0,0) 字符设备标记删除 |

**Phase 9 奠定的基础**：overlayfs 是容器存储的核心。有了它，Phase 10 的 namespace 隔离就有了文件系统隔离的基础。
