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

---

## 9.10 完整源码清单

> Phase 9 在 Phase 8 基础上新增 5 个源文件（`fs/overlayfs/` 目录），修改 `kernel/main.c` 和 `Makefile`。

### 新增目录结构

```
arm64os/
├── arch/arm64/
│   ├── include/asm/
│   │   ├── memory.h               (Phase 2, 不变)
│   │   ├── pgtable.h              (Phase 2, 不变)
│   │   └── sysreg.h               (Phase 2, 不变)
│   ├── kernel/
│   │   ├── head.S                  (Phase 1, 不变)
│   │   ├── entry.S                 (Phase 5 版本, 不变)
│   │   └── process.o               (Phase 5, 不变)
│   └── mm/
│       ├── mmu.c                   (Phase 2, 不变)
│       ├── proc.S                  (Phase 2, 不变)
│       └── tlb.S                   (Phase 2, 不变)
├── drivers/
│   ├── irqchip/
│   │   └── gic-v3.c                (Phase 3, 不变)
│   ├── timer/
│   │   └── arm_arch_timer.c        (Phase 3, 不变)
│   ├── virtio/
│   │   ├── virtio.c                (Phase 6, 不变)
│   │   ├── virtio_mmio.c           (Phase 6, 不变)
│   │   └── virtio_ring.c           (Phase 6, 不变)
│   ├── block/
│   │   └── virtio_blk.c            (Phase 6, 不变)
│   └── net/
│       └── virtio_net.c            (Phase 6, 不变)
├── fs/
│   ├── vfs/
│   │   ├── super.c                 (Phase 7, 不变)
│   │   ├── inode.c                 (Phase 7, 不变)
│   │   ├── dcache.c                (Phase 7, 不变)
│   │   ├── file.c                  (Phase 7, 不变)
│   │   └── namei.c                 (Phase 7, 不变)
│   ├── ramfs/
│   │   └── ramfs.c                 (Phase 7, 不变)
│   ├── squashfs/
│   │   ├── super.c                 (Phase 8, 不变)
│   │   ├── inode.c                 (Phase 8, 不变)
│   │   ├── dir.c                   (Phase 8, 不变)
│   │   ├── file.c                  (Phase 8, 不变)
│   │   └── decompressor.c          (Phase 8, 不变)
│   ├── xfs/
│   │   ├── xfs_super.c             (Phase 8, 不变)
│   │   ├── xfs_log.c               (Phase 8, 不变)
│   │   ├── xfs_inode.c             (Phase 8, 不变)
│   │   ├── xfs_alloc.c             (Phase 8, 不变)
│   │   └── xfs_dir2.c              (Phase 8, 不变)
│   └── overlayfs/
│       ├── super.c                 ← 新增（超级块与挂载）
│       ├── inode.c                 ← 新增（路径查找、inode 操作）
│       ├── dir.c                   ← 新增（目录操作、whiteout）
│       ├── file.c                  ← 新增（文件读写代理）
│       └── copy_up.c              ← 新增（写时复制核心）
├── include/linux/
│   ├── types.h                     (Phase 1, 不变)
│   ├── list.h                      (Phase 2, 不变)
│   ├── io.h                        (Phase 2, 不变)
│   ├── irq.h                       (Phase 3, 不变)
│   ├── sched.h                     (Phase 4, 不变)
│   └── fs.h                        (Phase 7, 不变)
├── kernel/
│   ├── irq/
│   │   ├── handle.c                (Phase 3, 不变)
│   │   └── irqdesc.c               (Phase 3, 不变)
│   ├── sched/
│   │   ├── fair.c                  (Phase 4, 不变)
│   │   └── core.c                  (Phase 4, 不变)
│   ├── syscall/
│   │   └── syscall.c               (Phase 5, 不变)
│   ├── fork.c                      (Phase 5, 不变)
│   ├── main.c                      ← 修改（Phase 9 初始化 + 测试）
│   └── printk.c                    (Phase 1, 不变)
├── lib/
│   └── rbtree.c                    (Phase 4, 不变)
├── mm/
│   ├── memblock.c                  (Phase 2, 不变)
│   └── page_alloc.c                (Phase 2, 不变)
├── scripts/linker.ld               (Phase 1, 不变)
└── Makefile                        ← 修改（新增 overlayfs 目标文件和编译规则）
```

### 新增文件 1: `fs/overlayfs/super.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/super.c
 *
 * overlayfs 超级块与挂载
 *
 * 参考：fs/overlayfs/super.c
 *
 * Phase 9 实现：
 *   - ovl_mount()：解析 lowerdir/upperdir/workdir 挂载选项
 *   - ovl_fill_super()：查找各层挂载点，构建 overlay 超级块
 *   - ovl_init()：注册 overlay 文件系统类型
 *
 * 简化说明：
 *   - 仅支持单个 lower 层（不支持多层 lower）
 *   - lower、upper、work 必须是已挂载的路径或根文件系统子目录
 *   - 选项字符串格式："lowerdir=/sq,upperdir=/upper,workdir=/work"
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct super_block *alloc_super(struct file_system_type *type);
struct inode *new_inode(struct super_block *sb);
struct dentry *d_alloc_root(struct super_block *sb);
int register_filesystem(struct file_system_type *fs);
struct dentry *path_lookup(const char *pathname);

/* overlayfs inode 操作（由 inode.c 提供） */
extern const struct inode_operations ovl_dir_inode_ops;
extern const struct file_operations  ovl_dir_fops;

/*
 * ============================================================
 * overlayfs 内部数据结构
 * ============================================================
 */

/* overlayfs 魔数 */
#define OVL_SUPER_MAGIC     0x794c7630UL    /* "ovl0" */

/*
 * ovl_fs - overlayfs 超级块私有数据
 *
 * 保存三层目录的 root dentry 引用。
 *
 * 参考：fs/overlayfs/ovl_entry.h struct ovl_fs
 */
struct ovl_fs {
    struct dentry  *lower_root;     /* lower 层根 dentry（squashfs） */
    struct dentry  *upper_root;     /* upper 层根 dentry（ramfs 子目录） */
    struct dentry  *work_root;      /* work 目录根 dentry */
};

/* ovl_fs 静态池 */
static struct ovl_fs ovl_fs_pool[2];
static int ovl_fs_idx = 0;

/*
 * ============================================================
 * 内部辅助函数：选项字符串解析
 * ============================================================
 */

/*
 * ovl_str_ncmp - 比较前 n 个字符
 */
static int ovl_str_ncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] - b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

/*
 * ovl_str_copy - 复制字符串
 */
static void ovl_str_copy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * ovl_parse_option - 解析单个选项 "key=value"
 *
 * 在 opts 字符串中查找 key=，提取 value 到 buf。
 * value 以 ',' 或 '\0' 结尾。
 *
 * @opts: 选项字符串（如 "lowerdir=/sq,upperdir=/upper,workdir=/work"）
 * @key:  选项名（如 "lowerdir="）
 * @klen: 选项名长度（含 '='）
 * @buf:  输出缓冲区
 * @blen: 缓冲区大小
 *
 * 返回 1 找到，0 未找到。
 */
static int ovl_parse_option(const char *opts, const char *key, int klen,
                             char *buf, int blen)
{
    const char *p = opts;
    int i;

    if (!opts)
        return 0;

    while (*p) {
        /* 检查是否匹配 key */
        if (ovl_str_ncmp(p, key, klen) == 0) {
            /* 提取 value */
            p += klen;
            for (i = 0; i < blen - 1 && p[i] && p[i] != ','; i++)
                buf[i] = p[i];
            buf[i] = '\0';
            return 1;
        }

        /* 跳到下一个选项 */
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }

    return 0;
}

/*
 * ============================================================
 * ovl_fill_super - 填充 overlayfs 超级块
 *
 * 解析挂载选项，查找各层路径对应的 dentry，
 * 创建 overlay 根目录 inode。
 *
 * @sb:   已分配的 VFS 超级块
 * @data: 挂载选项字符串
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/super.c ovl_fill_super()
 * ============================================================
 */
static int ovl_fill_super(struct super_block *sb, void *data)
{
    struct ovl_fs *ofs;
    struct inode *root_inode;
    struct dentry *root_dentry;
    char lower_path[32];
    char upper_path[32];
    char work_path[32];

    /* 分配 ovl_fs */
    if (ovl_fs_idx >= 2) {
        boot_printk("[overlayfs] ERROR: ovl_fs pool exhausted\n");
        return -1;
    }
    ofs = &ovl_fs_pool[ovl_fs_idx++];

    /* 解析挂载选项 */
    if (!ovl_parse_option((const char *)data, "lowerdir=", 9,
                           lower_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing lowerdir option\n");
        return -1;
    }
    if (!ovl_parse_option((const char *)data, "upperdir=", 9,
                           upper_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing upperdir option\n");
        return -1;
    }
    if (!ovl_parse_option((const char *)data, "workdir=", 8,
                           work_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing workdir option\n");
        return -1;
    }

    boot_printk("[overlayfs] lower=");
    boot_printk(lower_path);
    boot_printk(" upper=");
    boot_printk(upper_path);
    boot_printk(" work=");
    boot_printk(work_path);
    boot_printk("\n");

    /* 查找各层根 dentry */
    ofs->lower_root = path_lookup(lower_path);
    if (!ofs->lower_root || !ofs->lower_root->d_inode) {
        boot_printk("[overlayfs] ERROR: lower path not found: ");
        boot_printk(lower_path);
        boot_printk("\n");
        return -1;
    }

    ofs->upper_root = path_lookup(upper_path);
    if (!ofs->upper_root || !ofs->upper_root->d_inode) {
        boot_printk("[overlayfs] ERROR: upper path not found: ");
        boot_printk(upper_path);
        boot_printk("\n");
        return -1;
    }

    ofs->work_root = path_lookup(work_path);
    if (!ofs->work_root || !ofs->work_root->d_inode) {
        boot_printk("[overlayfs] ERROR: work path not found: ");
        boot_printk(work_path);
        boot_printk("\n");
        return -1;
    }

    /* 填充 VFS 超级块 */
    sb->s_magic = OVL_SUPER_MAGIC;
    sb->s_blocksize = 4096;
    sb->s_fs_info = ofs;

    /* 创建 overlay 根目录 inode */
    root_inode = new_inode(sb);
    if (!root_inode) {
        boot_printk("[overlayfs] ERROR: cannot create root inode\n");
        return -1;
    }

    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &ovl_dir_inode_ops;
    root_inode->i_fop = &ovl_dir_fops;

    /*
     * 根 inode 的 i_private 指向 ovl_fs，
     * 以便 lookup 时能找到 lower/upper 层。
     * 非根 inode 使用 ovl_inode_info（由 inode.c 分配）。
     */
    root_inode->i_private = ofs;

    /* 创建根 dentry */
    root_dentry = d_alloc_root(sb);
    if (!root_dentry) {
        boot_printk("[overlayfs] ERROR: cannot create root dentry\n");
        return -1;
    }
    root_dentry->d_inode = root_inode;
    sb->s_root = root_dentry;

    boot_printk("[overlayfs] Superblock filled, root ready\n");

    return 0;
}

/*
 * ============================================================
 * ovl_mount - overlayfs mount 回调
 *
 * 参考：fs/overlayfs/super.c ovl_mount()
 * ============================================================
 */
static struct dentry *ovl_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev_name, void *data)
{
    struct super_block *sb;
    int ret;

    boot_printk("[overlayfs] Mounting overlayfs...\n");

    sb = alloc_super(fs_type);
    if (!sb)
        return NULL;

    ret = ovl_fill_super(sb, data);
    if (ret != 0)
        return NULL;

    boot_printk("[overlayfs] overlayfs mounted successfully\n");

    return sb->s_root;
}

/*
 * ============================================================
 * ovl_kill_sb - 卸载回调
 * ============================================================
 */
static void ovl_kill_sb(struct super_block *sb)
{
    boot_printk("[overlayfs] Unmounting overlayfs\n");
    (void)sb;
}

/*
 * ============================================================
 * 文件系统类型注册
 * ============================================================
 */
static struct file_system_type ovl_fs_type = {
    .name    = "overlay",
    .mount   = ovl_mount,
    .kill_sb = ovl_kill_sb,
    .next    = NULL,
};

/*
 * ============================================================
 * ovl_init - 注册 overlay 文件系统
 *
 * 在 VFS 初始化后调用。
 *
 * 参考：fs/overlayfs/super.c init_overlayfs()
 * ============================================================
 */
void ovl_init(void)
{
    register_filesystem(&ovl_fs_type);
}
```

### 新增文件 2: `fs/overlayfs/inode.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/inode.c
 *
 * overlayfs inode 操作（路径查找、getattr）
 *
 * 参考：fs/overlayfs/inode.c
 *       fs/overlayfs/namei.c
 *
 * Phase 9 实现：
 *   - ovl_lookup()：在 upper 和 lower 层查找文件
 *   - ovl_alloc_inode_info()：分配 overlay inode 私有数据
 *   - ovl_create_inode()：创建 overlay inode 包装真实 inode
 *
 * 查找规则：
 *   1. 先在 upper 层查找
 *   2. 如果 upper 中是 whiteout（S_IFCHR），返回负 dentry
 *   3. 如果 upper 未找到，在 lower 层查找
 *   4. 创建 overlay inode 包装真实 inode
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct inode *new_inode(struct super_block *sb);
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);

/* copy-up 接口（由 copy_up.c 提供） */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry);

/* overlayfs file 操作（由 file.c 提供） */
extern const struct file_operations ovl_file_fops;

/* whiteout 创建（由 dir.c 提供） */
int ovl_do_whiteout(struct super_block *sb, const char *name, unsigned int namelen);

/*
 * ============================================================
 * ovl_inode_info - overlay inode 私有数据
 *
 * 包装真实 inode 的来源信息（upper 层或 lower 层）。
 *
 * 参考：fs/overlayfs/ovl_entry.h struct ovl_inode
 * ============================================================
 */
struct ovl_inode_info {
    struct dentry  *upper_dentry;   /* upper 层 dentry（NULL 如果不在 upper） */
    struct dentry  *lower_dentry;   /* lower 层 dentry（NULL 如果不在 lower） */
};

/* ovl_inode_info 静态池 */
#define MAX_OVL_INODES  64
static struct ovl_inode_info ovl_info_pool[MAX_OVL_INODES];
static int ovl_info_idx = 0;

/*
 * ============================================================
 * ovl_fs - overlayfs 超级块私有数据（与 super.c 共用定义）
 * ============================================================
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

/*
 * ============================================================
 * ovl_alloc_inode_info - 分配 overlay inode 私有数据
 * ============================================================
 */
static struct ovl_inode_info *ovl_alloc_inode_info(void)
{
    struct ovl_inode_info *info;

    if (ovl_info_idx >= MAX_OVL_INODES) {
        boot_printk("[overlayfs] ERROR: inode info pool exhausted\n");
        return NULL;
    }

    info = &ovl_info_pool[ovl_info_idx++];
    info->upper_dentry = NULL;
    info->lower_dentry = NULL;

    return info;
}

/*
 * ============================================================
 * ovl_lookup_in_layer - 在指定层的根目录中查找子条目
 *
 * 通过 dcache 快速路径和文件系统 lookup 慢速路径查找。
 *
 * @layer_root: 该层的根 dentry
 * @name:       文件名
 * @namelen:    文件名长度
 *
 * 返回找到的 dentry（可能 d_inode=NULL），未找到返回 NULL。
 *
 * 参考：fs/overlayfs/namei.c ovl_lookup_single()
 * ============================================================
 */
static struct dentry *ovl_lookup_in_layer(struct dentry *layer_root,
                                           const char *name,
                                           unsigned int namelen)
{
    struct qstr qname;
    struct dentry *child;
    struct inode *dir_inode;

    if (!layer_root || !layer_root->d_inode)
        return NULL;

    /* 构造 qstr */
    qname.name = name;
    qname.len = namelen;
    qname.hash = full_name_hash(layer_root, name, namelen);

    /* 1. dcache 快速路径 */
    child = d_lookup(layer_root, &qname);
    if (child)
        return child;

    /* 2. 文件系统 lookup 慢速路径 */
    dir_inode = layer_root->d_inode;
    if (!dir_inode->i_op || !dir_inode->i_op->lookup)
        return NULL;

    child = d_alloc(layer_root, &qname);
    if (!child)
        return NULL;

    child = dir_inode->i_op->lookup(dir_inode, child, 0);
    return child;
}

/*
 * ============================================================
 * ovl_lookup - overlayfs 路径查找
 *
 * 在 upper 层和 lower 层中查找文件名，创建 overlay inode。
 *
 * 查找规则（参考 fs/overlayfs/namei.c ovl_lookup()）：
 *   1. 先在 upper 层查找
 *      - 如果找到 whiteout（S_IFCHR），文件已被删除，返回负 dentry
 *      - 如果找到正常文件，使用 upper 层版本
 *   2. 如果 upper 未找到，在 lower 层查找
 *   3. 创建 overlay inode 包装真实 inode，设置操作集
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 待查找的 dentry（由 VFS 分配，name 已设置）
 * @flags:  查找标志
 *
 * 返回 dentry（找到则 d_inode 非 NULL），负 dentry 表示不存在。
 * ============================================================
 */
struct dentry *ovl_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct dentry *upper_child = NULL;
    struct dentry *lower_child = NULL;
    struct inode *ovl_inode;
    struct inode *real_inode = NULL;

    (void)flags;

    /*
     * 获取 ovl_fs：
     * 对于根目录，i_private 直接是 ovl_fs *。
     * 对于非根目录，需要从 sb->s_fs_info 获取。
     *
     * 当前简化版只支持平坦目录（不支持子目录嵌套），
     * 所以 dir 一定是 overlay 的根目录。
     */
    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;
    if (!ofs)
        return dentry;  /* 负 dentry */

    /* Step 1: 在 upper 层查找 */
    upper_child = ovl_lookup_in_layer(ofs->upper_root,
                                       dentry->d_name.name,
                                       dentry->d_name.len);

    if (upper_child && upper_child->d_inode) {
        /* 检查是否是 whiteout */
        if (S_ISCHR(upper_child->d_inode->i_mode)) {
            /*
             * whiteout：文件已被删除。
             * 返回负 dentry（d_inode = NULL）。
             */
            return dentry;
        }

        /* upper 层有此文件 */
        real_inode = upper_child->d_inode;
    }

    /* Step 2: 如果 upper 未找到，在 lower 层查找 */
    if (!real_inode) {
        lower_child = ovl_lookup_in_layer(ofs->lower_root,
                                           dentry->d_name.name,
                                           dentry->d_name.len);

        if (lower_child && lower_child->d_inode) {
            real_inode = lower_child->d_inode;
        }
    }

    /* 没有找到 */
    if (!real_inode)
        return dentry;  /* 负 dentry */

    /* Step 3: 创建 overlay inode */
    oi = ovl_alloc_inode_info();
    if (!oi)
        return dentry;

    oi->upper_dentry = (upper_child && upper_child->d_inode &&
                         !S_ISCHR(upper_child->d_inode->i_mode))
                        ? upper_child : NULL;
    oi->lower_dentry = (lower_child && lower_child->d_inode)
                        ? lower_child : NULL;

    ovl_inode = new_inode(dentry->d_sb);
    if (!ovl_inode)
        return dentry;

    /* 复制真实 inode 的元数据 */
    ovl_inode->i_mode = real_inode->i_mode;
    ovl_inode->i_size = real_inode->i_size;
    ovl_inode->i_private = oi;

    /* 设置操作集 */
    if (S_ISDIR(real_inode->i_mode)) {
        extern const struct inode_operations ovl_dir_inode_ops;
        extern const struct file_operations  ovl_dir_fops;
        ovl_inode->i_op = &ovl_dir_inode_ops;
        ovl_inode->i_fop = &ovl_dir_fops;
    } else {
        extern const struct inode_operations ovl_file_inode_ops;
        ovl_inode->i_op = &ovl_file_inode_ops;
        ovl_inode->i_fop = &ovl_file_fops;
    }

    /* 关联 inode 到 dentry 并加入 dcache */
    d_add(dentry, ovl_inode);

    return dentry;
}

/*
 * ============================================================
 * ovl_create - 在 overlay 中创建新文件
 *
 * 新文件直接创建在 upper 层。
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 新文件的 dentry
 * @mode:   文件权限
 * @excl:   排他创建
 *
 * 参考：fs/overlayfs/dir.c ovl_create()
 * ============================================================
 */
static int ovl_create(struct inode *dir, struct dentry *dentry,
                        unsigned int mode, bool excl)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct inode *upper_dir;
    struct dentry *upper_child;
    struct qstr qname;
    struct inode *ovl_inode;
    int ret;

    (void)excl;

    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;
    if (!ofs || !ofs->upper_root || !ofs->upper_root->d_inode)
        return -1;

    upper_dir = ofs->upper_root->d_inode;
    if (!upper_dir->i_op || !upper_dir->i_op->create)
        return -1;

    /* 在 upper 层分配 dentry */
    qname.name = dentry->d_name.name;
    qname.len = dentry->d_name.len;
    qname.hash = full_name_hash(ofs->upper_root,
                                 dentry->d_name.name,
                                 dentry->d_name.len);

    upper_child = d_alloc(ofs->upper_root, &qname);
    if (!upper_child)
        return -12;

    /* 在 upper 层创建文件 */
    ret = upper_dir->i_op->create(upper_dir, upper_child, mode, false);
    if (ret != 0)
        return ret;

    /* 创建 overlay inode 包装 */
    oi = ovl_alloc_inode_info();
    if (!oi)
        return -12;

    oi->upper_dentry = upper_child;
    oi->lower_dentry = NULL;

    ovl_inode = new_inode(dentry->d_sb);
    if (!ovl_inode)
        return -12;

    ovl_inode->i_mode = upper_child->d_inode->i_mode;
    ovl_inode->i_size = upper_child->d_inode->i_size;
    ovl_inode->i_private = oi;

    {
        extern const struct inode_operations ovl_file_inode_ops;
        ovl_inode->i_op = &ovl_file_inode_ops;
    }
    ovl_inode->i_fop = &ovl_file_fops;

    d_add(dentry, ovl_inode);

    return 0;
}

/*
 * ============================================================
 * ovl_unlink - 删除 overlay 中的文件
 *
 * 如果文件在 upper 层：直接从 upper 删除。
 * 如果文件只在 lower 层：在 upper 层创建 whiteout。
 * 如果文件在两层都有（copy-up 过的）：删除 upper 并创建 whiteout。
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 要删除的 overlay dentry
 *
 * 参考：fs/overlayfs/dir.c ovl_unlink()
 * ============================================================
 */
static int ovl_unlink(struct inode *dir, struct dentry *dentry)
{
    struct ovl_inode_info *oi;
    struct ovl_fs *ofs;
    int need_whiteout;

    if (!dentry->d_inode)
        return -2;  /* ENOENT */

    oi = (struct ovl_inode_info *)dentry->d_inode->i_private;
    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;

    if (!oi || !ofs)
        return -1;

    /*
     * 需要 whiteout 的情况：
     * - 文件在 lower 层存在（需要屏蔽 lower 层的文件）
     */
    need_whiteout = (oi->lower_dentry != NULL);

    if (need_whiteout) {
        /* 在 upper 层创建 whiteout */
        int ret = ovl_do_whiteout(dir->i_sb,
                                   dentry->d_name.name,
                                   dentry->d_name.len);
        if (ret != 0)
            return ret;
    }

    /* 使 overlay dentry 变为负 dentry（文件"消失"） */
    dentry->d_inode = NULL;

    boot_printk("[overlayfs] unlink: ");
    boot_printk(dentry->d_name.name);
    if (need_whiteout)
        boot_printk(" (whiteout created)");
    boot_printk("\n");

    return 0;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* overlayfs 目录 inode 操作集 */
const struct inode_operations ovl_dir_inode_ops = {
    .lookup     = ovl_lookup,
    .create     = ovl_create,
    .mkdir      = NULL,         /* Phase 9 不支持 mkdir on overlay */
    .unlink     = ovl_unlink,
};

/* overlayfs 文件 inode 操作集 */
const struct inode_operations ovl_file_inode_ops = {
    .lookup     = NULL,
    .create     = NULL,
    .mkdir      = NULL,
    .unlink     = NULL,
};

/* overlayfs 目录 file 操作集 */
const struct file_operations ovl_dir_fops = {
    .read       = NULL,
    .write      = NULL,
    .open       = NULL,
    .release    = NULL,
};
```

### 新增文件 3: `fs/overlayfs/dir.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/dir.c
 *
 * overlayfs 目录操作、whiteout 处理
 *
 * 参考：fs/overlayfs/dir.c
 *
 * Phase 9 实现：
 *   - ovl_do_whiteout()：在 upper 层创建 whiteout 文件
 *   - ovl_is_whiteout()：检查 dentry 是否是 whiteout
 *
 * whiteout 原理：
 *   - overlayfs 无法修改 lower 层（只读）
 *   - 删除文件时，在 upper 层创建同名的 whiteout 特殊文件
 *   - whiteout 是字符设备文件，主/次设备号 (0, 0)
 *   - 路径查找遇到 whiteout 即视为文件不存在
 *   - 实际 Linux 内核使用 mknod 创建 S_IFCHR 设备节点
 *
 * 简化说明：
 *   - whiteout 通过 ramfs_create + 设置 i_mode = S_IFCHR 实现
 *   - 不支持目录级 whiteout（opaque 标记）
 *   - 不支持目录合并 readdir（Phase 9 简化版）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);
struct inode *new_inode(struct super_block *sb);

/*
 * ============================================================
 * whiteout 常量
 *
 * 参考：include/linux/fs.h, fs/overlayfs/overlayfs.h
 * ============================================================
 */
#define WHITEOUT_MODE       (S_IFCHR)       /* 字符设备类型 */
#define WHITEOUT_DEV        0               /* 设备号 (0, 0) */

/*
 * ovl_fs 定义（与 super.c 共用）
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

/*
 * ============================================================
 * ovl_is_whiteout - 检查 dentry 是否是 whiteout
 *
 * whiteout 判定条件：
 *   - inode 类型为 S_IFCHR（字符设备）
 *   - 在真实 Linux 中还检查 MAJOR(rdev)==0 && MINOR(rdev)==0
 *   - 我们的简化版只检查 S_IFCHR
 *
 * @dentry: 要检查的 dentry
 *
 * 返回 1 是 whiteout，0 不是。
 *
 * 参考：fs/overlayfs/util.c ovl_is_whiteout()
 * ============================================================
 */
int ovl_is_whiteout(struct dentry *dentry)
{
    if (!dentry || !dentry->d_inode)
        return 0;

    return S_ISCHR(dentry->d_inode->i_mode);
}

/*
 * ============================================================
 * ovl_do_whiteout - 在 upper 层创建 whiteout 文件
 *
 * 流程：
 *   1. 在 upper 层的根目录下分配 dentry
 *   2. 通过 upper 层文件系统的 create 创建文件
 *   3. 将 inode 的 mode 设置为 S_IFCHR（字符设备）
 *
 * 这样 overlayfs 在 lookup 时遇到此 inode，
 * ovl_is_whiteout() 返回 true，文件视为已删除。
 *
 * @sb:      overlay 超级块
 * @name:    文件名
 * @namelen: 文件名长度
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/dir.c ovl_whiteout()
 * ============================================================
 */
int ovl_do_whiteout(struct super_block *sb, const char *name,
                      unsigned int namelen)
{
    struct ovl_fs *ofs;
    struct inode *upper_dir;
    struct dentry *wh_dentry;
    struct qstr qname;
    int ret;

    ofs = (struct ovl_fs *)sb->s_fs_info;
    if (!ofs || !ofs->upper_root || !ofs->upper_root->d_inode)
        return -1;

    upper_dir = ofs->upper_root->d_inode;
    if (!upper_dir->i_op || !upper_dir->i_op->create)
        return -1;

    /* 检查 upper 层是否已有同名文件 */
    qname.name = name;
    qname.len = namelen;
    qname.hash = full_name_hash(ofs->upper_root, name, namelen);

    wh_dentry = d_lookup(ofs->upper_root, &qname);

    if (wh_dentry && wh_dentry->d_inode) {
        /*
         * upper 层已有此文件（可能是 copy-up 过的）。
         * 直接将其转为 whiteout：修改 inode mode。
         */
        wh_dentry->d_inode->i_mode = WHITEOUT_MODE;
        wh_dentry->d_inode->i_size = 0;
        return 0;
    }

    /* upper 层无此文件，创建新的 whiteout */
    wh_dentry = d_alloc(ofs->upper_root, &qname);
    if (!wh_dentry)
        return -12;

    /*
     * 使用 upper 层文件系统的 create 创建文件。
     * 传入 S_IFREG 让 ramfs_create 正常工作（分配 inode + 数据结构），
     * 然后我们手动修改 mode 为 S_IFCHR。
     */
    ret = upper_dir->i_op->create(upper_dir, wh_dentry,
                                    S_IFREG | 0000, false);
    if (ret != 0)
        return ret;

    /* 将 mode 改为字符设备（whiteout 标记） */
    if (wh_dentry->d_inode) {
        wh_dentry->d_inode->i_mode = WHITEOUT_MODE;
        wh_dentry->d_inode->i_size = 0;
        wh_dentry->d_inode->i_fop = NULL;  /* whiteout 不可读写 */
    }

    boot_printk("[overlayfs] whiteout created: ");
    {
        unsigned int i;
        char nbuf[32];
        for (i = 0; i < namelen && i < 31; i++)
            nbuf[i] = name[i];
        nbuf[i] = '\0';
        boot_printk(nbuf);
    }
    boot_printk("\n");

    return 0;
}
```

### 新增文件 4: `fs/overlayfs/file.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/file.c
 *
 * overlayfs 文件读写（透明代理）
 *
 * 参考：fs/overlayfs/file.c
 *
 * Phase 9 实现：
 *   - ovl_read()：从真实层读取数据
 *   - ovl_write()：写入数据（先 copy-up 再写入）
 *   - ovl_open()：打开时初始化层信息
 *
 * 读写代理原理：
 *   - overlay 的 file 对象存储对真实文件系统 inode 的引用
 *   - read：优先从 upper 层读取，否则从 lower 层读取
 *   - write：如果文件在 lower 层，先执行 copy-up 到 upper 层，
 *            然后在 upper 层执行写入
 *
 * 简化说明：
 *   - 使用 file->f_inode->i_private 中的 ovl_inode_info 访问真实层
 *   - 直接调用底层文件系统的 read/write 回调
 *   - copy-up 在首次写入时触发（COW — Copy on Write）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* copy-up 接口（由 copy_up.c 提供） */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry);

/*
 * ovl_inode_info 定义（与 inode.c 共用）
 */
struct ovl_inode_info {
    struct dentry  *upper_dentry;
    struct dentry  *lower_dentry;
};

/*
 * ============================================================
 * ovl_get_real_inode - 获取真实层 inode
 *
 * 优先返回 upper 层 inode（修改过的版本），
 * 否则返回 lower 层 inode（原始版本）。
 *
 * @ovl_inode: overlay inode
 *
 * 返回真实层 inode，NULL 表示无有效层。
 * ============================================================
 */
static struct inode *ovl_get_real_inode(struct inode *ovl_inode)
{
    struct ovl_inode_info *oi;

    if (!ovl_inode || !ovl_inode->i_private)
        return NULL;

    oi = (struct ovl_inode_info *)ovl_inode->i_private;

    /* upper 优先 */
    if (oi->upper_dentry && oi->upper_dentry->d_inode)
        return oi->upper_dentry->d_inode;

    /* 否则 lower */
    if (oi->lower_dentry && oi->lower_dentry->d_inode)
        return oi->lower_dentry->d_inode;

    return NULL;
}

/*
 * ============================================================
 * ovl_read - overlayfs 读操作
 *
 * 从真实层的文件系统读取数据。
 * 不触发 copy-up（只读操作直接透传）。
 *
 * @filp:  overlay file 对象
 * @buf:   目标缓冲区
 * @count: 请求读取的字节数
 * @pos:   读写偏移指针
 *
 * 返回实际读取的字节数，负数表示错误。
 *
 * 参考：fs/overlayfs/file.c ovl_read_iter()
 * ============================================================
 */
static ssize_t ovl_read(struct file *filp, char *buf, size_t count,
                          unsigned long *pos)
{
    struct inode *real_inode;
    struct file fake_file;

    real_inode = ovl_get_real_inode(filp->f_inode);
    if (!real_inode || !real_inode->i_fop || !real_inode->i_fop->read)
        return -9;  /* EBADF */

    /*
     * 构造临时 file 对象指向真实 inode。
     * 底层文件系统的 read 回调通过 filp->f_inode 访问数据，
     * 所以我们需要让它看到真实 inode。
     *
     * 参考：fs/overlayfs/file.c ovl_real_file()
     */
    fake_file.f_dentry = NULL;
    fake_file.f_inode = real_inode;
    fake_file.f_op = real_inode->i_fop;
    fake_file.f_flags = filp->f_flags;
    fake_file.f_pos = *pos;
    fake_file.f_count = 1;

    {
        ssize_t ret = real_inode->i_fop->read(&fake_file, buf, count,
                                               &fake_file.f_pos);
        /* 同步偏移回 overlay file */
        *pos = fake_file.f_pos;
        return ret;
    }
}

/*
 * ============================================================
 * ovl_write - overlayfs 写操作
 *
 * 写入前检查是否需要 copy-up：
 *   - 如果文件只在 lower 层，先 copy-up 到 upper 层
 *   - 然后在 upper 层执行写入
 *
 * @filp:  overlay file 对象
 * @buf:   数据缓冲区
 * @count: 写入字节数
 * @pos:   读写偏移指针
 *
 * 返回实际写入的字节数，负数表示错误。
 *
 * 参考：fs/overlayfs/file.c ovl_write_iter()
 * ============================================================
 */
static ssize_t ovl_write(struct file *filp, const char *buf, size_t count,
                           unsigned long *pos)
{
    struct ovl_inode_info *oi;
    struct inode *real_inode;
    struct file fake_file;

    if (!filp->f_inode || !filp->f_inode->i_private)
        return -9;  /* EBADF */

    oi = (struct ovl_inode_info *)filp->f_inode->i_private;

    /*
     * 检查 copy-up 需要：
     * 如果文件在 lower 层但不在 upper 层，需要先 copy-up。
     */
    if (!oi->upper_dentry && oi->lower_dentry) {
        int ret;

        boot_printk("[overlayfs] write triggers copy-up\n");

        ret = ovl_copy_up_one(filp->f_inode->i_sb, filp->f_dentry);
        if (ret != 0) {
            boot_printk("[overlayfs] ERROR: copy-up failed\n");
            return ret;
        }

        /* copy-up 后 oi->upper_dentry 应该已被设置 */

        /*
         * copy-up 后截断 upper 文件：
         * copy-up 复制了 lower 的全部内容到 upper，但调用者即将从 pos 0
         * 写入新数据。如果新数据比原文件短，尾部会残留旧数据。
         * 简化处理：copy-up 后重置 upper i_size 为 0，让写入从空文件开始。
         * （真实 Linux 中由 O_TRUNC 或 ftruncate 处理）
         */
        if (oi->upper_dentry && oi->upper_dentry->d_inode)
            oi->upper_dentry->d_inode->i_size = 0;
    }

    /* 获取 upper 层 inode 进行写入 */
    if (!oi->upper_dentry || !oi->upper_dentry->d_inode) {
        boot_printk("[overlayfs] ERROR: no upper inode after copy-up\n");
        return -1;
    }

    real_inode = oi->upper_dentry->d_inode;
    if (!real_inode->i_fop || !real_inode->i_fop->write)
        return -9;

    /* 构造临时 file 指向 upper 层真实 inode */
    fake_file.f_dentry = oi->upper_dentry;
    fake_file.f_inode = real_inode;
    fake_file.f_op = real_inode->i_fop;
    fake_file.f_flags = filp->f_flags;
    fake_file.f_pos = *pos;
    fake_file.f_count = 1;

    {
        ssize_t ret = real_inode->i_fop->write(&fake_file, buf, count,
                                                &fake_file.f_pos);
        /* 同步偏移和大小 */
        *pos = fake_file.f_pos;
        filp->f_inode->i_size = real_inode->i_size;
        return ret;
    }
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* overlayfs 文件操作集 */
const struct file_operations ovl_file_fops = {
    .read       = ovl_read,
    .write      = ovl_write,
    .open       = NULL,
    .release    = NULL,
};
```

### 新增文件 5: `fs/overlayfs/copy_up.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/copy_up.c
 *
 * overlayfs copy-up 机制核心
 *
 * 参考：fs/overlayfs/copy_up.c
 *
 * Phase 9 实现：
 *   - ovl_copy_up_one()：将文件从 lower 层复制到 upper 层
 *
 * copy-up 原理（写时复制 COW）：
 *   当用户写入一个只存在于 lower 层（只读）的文件时：
 *   1. 在 work 目录创建临时文件（原子性保证）
 *   2. 从 lower 层读取文件全部内容
 *   3. 将内容写入临时文件
 *   4. 复制文件元数据（权限、大小等）
 *   5. 原子 rename：work/tmp → upper/filename
 *   6. 更新 overlay inode，后续操作走 upper 层
 *
 * 简化说明：
 *   - 跳过 work 目录原子 rename，直接在 upper 创建
 *     （我们的内核不支持 rename，且单 CPU 无并发问题）
 *   - 不复制 xattr、安全标签等
 *   - 文件最大 4KB（受 ramfs 单页限制）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);
struct inode *new_inode(struct super_block *sb);

/*
 * 与其他 overlayfs 源文件共用的数据结构定义
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

struct ovl_inode_info {
    struct dentry  *upper_dentry;
    struct dentry  *lower_dentry;
};

/* copy-up 临时缓冲区（4KB，页对齐） */
static char __attribute__((aligned(4096))) copyup_buf[4096];

/*
 * ============================================================
 * ovl_copy_data - 从 lower inode 读取数据到缓冲区
 *
 * @lower_inode: lower 层文件 inode
 * @buf:         目标缓冲区
 * @size:        要复制的字节数
 *
 * 返回实际读取的字节数，负数表示错误。
 * ============================================================
 */
static ssize_t ovl_copy_data(struct inode *lower_inode, char *buf,
                               unsigned long size)
{
    struct file fake_file;
    unsigned long pos = 0;

    if (!lower_inode->i_fop || !lower_inode->i_fop->read)
        return -1;

    /* 构造临时 file 用于底层读取 */
    fake_file.f_dentry = NULL;
    fake_file.f_inode = lower_inode;
    fake_file.f_op = lower_inode->i_fop;
    fake_file.f_flags = 0;     /* O_RDONLY */
    fake_file.f_pos = 0;
    fake_file.f_count = 1;

    return lower_inode->i_fop->read(&fake_file, buf, size, &pos);
}

/*
 * ============================================================
 * ovl_write_data - 将数据写入 upper inode
 *
 * @upper_inode: upper 层文件 inode
 * @buf:         源数据
 * @size:        写入字节数
 *
 * 返回实际写入的字节数，负数表示错误。
 * ============================================================
 */
static ssize_t ovl_write_data(struct inode *upper_inode,
                                struct dentry *upper_dentry,
                                const char *buf, unsigned long size)
{
    struct file fake_file;
    unsigned long pos = 0;

    if (!upper_inode->i_fop || !upper_inode->i_fop->write)
        return -1;

    /* 构造临时 file 用于底层写入 */
    fake_file.f_dentry = upper_dentry;
    fake_file.f_inode = upper_inode;
    fake_file.f_op = upper_inode->i_fop;
    fake_file.f_flags = 1;     /* O_WRONLY */
    fake_file.f_pos = 0;
    fake_file.f_count = 1;

    return upper_inode->i_fop->write(&fake_file, buf, size, &pos);
}

/*
 * ============================================================
 * ovl_copy_up_one - copy-up 核心：将文件从 lower 复制到 upper
 *
 * 完整流程：
 *   1. 获取 lower 层文件数据（读取）
 *   2. 在 upper 层根目录创建同名文件
 *   3. 将数据写入 upper 层文件
 *   4. 复制元数据（mode、size）
 *   5. 更新 overlay inode 的 upper_dentry
 *
 * @sb:         overlay 超级块
 * @ovl_dentry: overlay dentry（包含文件名）
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/copy_up.c ovl_copy_up_one()
 * ============================================================
 */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct inode *lower_inode;
    struct inode *upper_dir;
    struct dentry *upper_child;
    struct qstr qname;
    ssize_t data_size;
    ssize_t written;
    int ret;

    ofs = (struct ovl_fs *)sb->s_fs_info;
    if (!ofs)
        return -1;

    /* 获取 overlay inode info */
    if (!ovl_dentry->d_inode || !ovl_dentry->d_inode->i_private)
        return -1;

    oi = (struct ovl_inode_info *)ovl_dentry->d_inode->i_private;

    /* 验证 lower 层存在 */
    if (!oi->lower_dentry || !oi->lower_dentry->d_inode) {
        boot_printk("[overlayfs] copy-up: no lower inode\n");
        return -1;
    }
    lower_inode = oi->lower_dentry->d_inode;

    /* 验证 upper 层可写 */
    if (!ofs->upper_root || !ofs->upper_root->d_inode) {
        boot_printk("[overlayfs] copy-up: no upper root\n");
        return -1;
    }
    upper_dir = ofs->upper_root->d_inode;

    if (!upper_dir->i_op || !upper_dir->i_op->create) {
        boot_printk("[overlayfs] copy-up: upper doesn't support create\n");
        return -1;
    }

    boot_printk("[overlayfs] copy-up: ");
    boot_printk(ovl_dentry->d_name.name);
    boot_printk(" (size=");
    boot_printk_hex(lower_inode->i_size);
    boot_printk(")\n");

    /* Step 1: 从 lower 层读取文件数据 */
    data_size = 0;
    if (lower_inode->i_size > 0) {
        if (lower_inode->i_size > 4096) {
            boot_printk("[overlayfs] copy-up: file too large (max 4KB)\n");
            return -1;
        }

        data_size = ovl_copy_data(lower_inode, copyup_buf,
                                    lower_inode->i_size);
        if (data_size < 0) {
            boot_printk("[overlayfs] copy-up: read lower failed\n");
            return -1;
        }
    }

    /* Step 2: 在 upper 层创建同名文件 */
    qname.name = ovl_dentry->d_name.name;
    qname.len = ovl_dentry->d_name.len;
    qname.hash = full_name_hash(ofs->upper_root,
                                 ovl_dentry->d_name.name,
                                 ovl_dentry->d_name.len);

    upper_child = d_alloc(ofs->upper_root, &qname);
    if (!upper_child) {
        boot_printk("[overlayfs] copy-up: d_alloc failed\n");
        return -12;
    }

    ret = upper_dir->i_op->create(upper_dir, upper_child,
                                    lower_inode->i_mode, false);
    if (ret != 0) {
        boot_printk("[overlayfs] copy-up: create in upper failed\n");
        return ret;
    }

    /* Step 3: 将数据写入 upper 层 */
    if (data_size > 0 && upper_child->d_inode) {
        written = ovl_write_data(upper_child->d_inode,
                                  upper_child,
                                  copyup_buf,
                                  (unsigned long)data_size);
        if (written != data_size) {
            boot_printk("[overlayfs] copy-up: write upper failed\n");
            return -1;
        }
    }

    /* Step 4: 更新 overlay inode */
    oi->upper_dentry = upper_child;

    /* 更新 overlay inode 的 size（现在等于 upper 的） */
    if (upper_child->d_inode)
        ovl_dentry->d_inode->i_size = upper_child->d_inode->i_size;

    boot_printk("[overlayfs] copy-up complete: ");
    boot_printk(ovl_dentry->d_name.name);
    boot_printk(" (");
    boot_printk_hex((unsigned long)data_size);
    boot_printk(" bytes copied)\n");

    return 0;
}
```

### 修改文件: `kernel/main.c` (Phase 9 新增部分)

> Phase 9 在 `kernel/main.c` 中新增 overlayfs 初始化调用和 `test_phase9()` 验证函数。

#### start_kernel() 中新增的 Phase 9 初始化代码

```c
    /* ---- Phase 9: overlayfs 三层联合挂载 ---- */
    /*
     * Phase 9 初始化顺序：
     *   1. 注册 overlay 文件系统类型
     *   2. 在 ramfs 根上创建 /upper 和 /work 目录
     *   3. 挂载 overlayfs 到 /merged（lower=/sq, upper=/upper, work=/work）
     *   4. 运行验证测试：读穿、copy-up、whiteout
     */
    boot_printk("[BOOT] === Phase 9: overlayfs ===\n");

    boot_printk("[BOOT] Registering overlayfs...\n");
    ovl_init();

    /* 运行 Phase 9 测试 */
    test_phase9();

    boot_printk("[BOOT] Phase 9 complete\n");
```

#### test_phase9() 验证函数

```c
/*
 * ============================================================
 * Phase 9: overlayfs 验证
 *
 * 流程：
 *   1. 在 ramfs 根上创建 /upper 和 /work 目录
 *   2. 挂载 overlayfs 到 /merged（lower=/sq, upper=/upper, work=/work）
 *   3. 读穿测试：读 /merged/hello.txt → 来自 lower 层 squashfs
 *   4. copy-up 测试：写 /merged/hello.txt → 触发 copy-up → 修改后内容来自 upper
 *   5. whiteout 测试：删除 /merged/readme.txt → 创建 whiteout → 文件不可见
 *
 * 参考：Phase 9 设计文档 §9.8
 * ============================================================
 */
static void test_phase9(void)
{
    int fd;
    char buf[64];
    ssize_t n;
    struct file *filp;
    int i;

    boot_printk("[BOOT] === Phase 9: overlayfs test ===\n");

    /* === Step 1: 创建 upper 和 work 目录 === */
    /*
     * ramfs（根文件系统）支持 mkdir。
     * 通过 VFS 接口创建 /upper 和 /work 目录。
     */
    boot_printk("[p9-test] Creating /upper and /work dirs...\n");
    {
        struct dentry *root_dentry;
        struct dentry *dir_dentry;
        struct qstr dir_name;
        struct inode *root_inode;
        int ret;

        root_dentry = path_lookup("/");
        if (!root_dentry || !root_dentry->d_inode) {
            boot_printk("[p9-test] FAIL: root lookup failed\n");
            return;
        }
        root_inode = root_dentry->d_inode;

        /* 创建 /upper */
        dir_name.name = "upper";
        dir_name.len = 5;
        dir_name.hash = full_name_hash(root_dentry, "upper", 5);

        dir_dentry = d_alloc(root_dentry, &dir_name);
        if (!dir_dentry) {
            boot_printk("[p9-test] FAIL: d_alloc /upper\n");
            return;
        }

        ret = root_inode->i_op->mkdir(root_inode, dir_dentry, 0755);
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: mkdir /upper\n");
            return;
        }

        /* 创建 /work */
        dir_name.name = "work";
        dir_name.len = 4;
        dir_name.hash = full_name_hash(root_dentry, "work", 4);

        dir_dentry = d_alloc(root_dentry, &dir_name);
        if (!dir_dentry) {
            boot_printk("[p9-test] FAIL: d_alloc /work\n");
            return;
        }

        ret = root_inode->i_op->mkdir(root_inode, dir_dentry, 0755);
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: mkdir /work\n");
            return;
        }
    }
    boot_printk("[p9-test] /upper and /work created\n");

    /* === Step 2: 挂载 overlayfs === */
    boot_printk("[p9-test] Mounting overlayfs on /merged...\n");
    if (do_mount("overlay", "/merged", "overlay", 0,
                  "lowerdir=/sq,upperdir=/upper,workdir=/work") != 0) {
        boot_printk("[p9-test] FAIL: mount overlayfs\n");
        return;
    }

    /* === Step 3: 读穿测试 === */
    /*
     * 读取 /merged/hello.txt — 文件只存在于 lower 层（squashfs）。
     * overlayfs 应透传到 squashfs 读取。
     * 预期内容："squashfs works!\n"（16 字节）
     */
    boot_printk("[p9-test] Read-through: /merged/hello.txt...\n");
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: open /merged/hello.txt, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    if (!filp) {
        boot_printk("[p9-test] FAIL: fget returned NULL\n");
        return;
    }

    for (i = 0; i < 64; i++)
        buf[i] = 0;

    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    boot_printk("[p9-test] read-through: ");
    boot_printk_hex((unsigned long)n);
    boot_printk(" bytes\n");

    if (n == 16 && vfs_str_equal(buf, "squashfs works!\n", 16)) {
        boot_printk("[p9-test] read-through: PASS\n");
    } else {
        boot_printk("[p9-test] FAIL: read-through content mismatch\n");
        buf[32] = '\0';
        boot_printk("[p9-test] got: ");
        boot_printk(buf);
        boot_printk("\n");
        return;
    }

    /* === Step 4: copy-up + write 测试 === */
    /*
     * 写入 /merged/hello.txt：
     *   - 文件在 lower 层（只读）
     *   - overlayfs 先执行 copy-up（复制到 upper 层）
     *   - 然后在 upper 层执行写入
     *   - 写入后文件大小变为新内容长度
     */
    boot_printk("[p9-test] Copy-up + write: /merged/hello.txt...\n");
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_WRONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: open for write, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    n = vfs_write(filp, "overlayfs!\n", 11);
    do_sys_close(&init_files, fd);

    if (n != 11) {
        boot_printk("[p9-test] FAIL: write returned ");
        boot_printk_hex((unsigned long)n);
        boot_printk("\n");
        return;
    }

    /* 验证 copy-up 后读取到修改后的内容 */
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: reopen after write\n");
        return;
    }

    filp = fget(&init_files, fd);
    for (i = 0; i < 64; i++)
        buf[i] = 0;
    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    if (n == 11 && vfs_str_equal(buf, "overlayfs!\n", 11)) {
        boot_printk("[p9-test] copy-up write+read: PASS\n");
    } else {
        boot_printk("[p9-test] FAIL: copy-up content mismatch (read ");
        boot_printk_hex((unsigned long)n);
        boot_printk(" bytes)\n");
        buf[32] = '\0';
        boot_printk("[p9-test] got: ");
        boot_printk(buf);
        boot_printk("\n");
        return;
    }

    /* === Step 5: whiteout 测试 === */
    /*
     * 删除 /merged/readme.txt：
     *   - 文件只在 lower 层（squashfs，只读）
     *   - overlayfs 无法修改 lower，在 upper 层创建 whiteout
     *   - whiteout 是 S_IFCHR 类型的特殊文件
     *   - 之后 lookup 遇到 whiteout 认为文件不存在
     */
    boot_printk("[p9-test] Whiteout: unlinking /merged/readme.txt...\n");

    /* 先验证文件存在 */
    fd = do_sys_open(&init_files, "/merged/readme.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: readme.txt not accessible before unlink\n");
        return;
    }
    do_sys_close(&init_files, fd);

    /* 执行 unlink（创建 whiteout） */
    {
        int ret = do_sys_unlink(&init_files, "/merged/readme.txt");
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: unlink returned ");
            boot_printk_hex((unsigned long)ret);
            boot_printk("\n");
            return;
        }
    }

    /* 验证文件不再可见 */
    fd = do_sys_open(&init_files, "/merged/readme.txt", O_RDONLY, 0);
    if (fd < 0) {
        /* 预期失败（ENOENT）— whiteout 生效 */
        boot_printk("[p9-test] whiteout: PASS (readme.txt hidden)\n");
    } else {
        boot_printk("[p9-test] FAIL: readme.txt still visible after whiteout\n");
        do_sys_close(&init_files, fd);
        return;
    }

    boot_printk("[BOOT] Phase 9 overlayfs tests: all passed\n");
}
```

### 修改文件: `Makefile` (Phase 9 新增部分)

> OBJS 新增 5 个 overlayfs 目标文件，新增 1 条编译规则。

OBJS 新增:
```makefile
    fs/overlayfs/super.o \
    fs/overlayfs/inode.o \
    fs/overlayfs/dir.o \
    fs/overlayfs/file.o \
    fs/overlayfs/copy_up.o \
```

新增编译规则:
```makefile
# Phase 9: fs/overlayfs/ C 文件
fs/overlayfs/%.o: fs/overlayfs/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

### 编译运行

```bash
# 创建新目录
mkdir -p fs/overlayfs

make clean && make
make run
# 期望输出包含：
#   [BOOT] === Phase 9: overlayfs ===
#   [BOOT] Registering overlayfs...
#   [overlayfs] overlayfs mounted successfully
#   [p9-test] read-through: PASS
#   [p9-test] copy-up write+read: PASS
#   [p9-test] whiteout: PASS (readme.txt hidden)
#   [BOOT] Phase 9 complete
# 按 Ctrl-A X 退出 QEMU
```
