# Phase 9：overlayfs三层联合挂载

## 参考内核文件

```
fs/overlayfs/super.c        # overlayfs挂载与超级块
fs/overlayfs/inode.c        # overlayfs inode操作
fs/overlayfs/dir.c          # 目录合并与 whiteout 处理
fs/overlayfs/file.c         # 文件读写（含 copy-up）
fs/overlayfs/copy_up.c      # copy-up 机制核心
fs/overlayfs/util.c         # 工具函数
include/linux/ovl_whiteout.h # whiteout 常量定义
```

---

## 9.1 overlayfs整体架构

overlayfs 将三层目录合并为一个统一视图，是容器存储的核心：

```
用户进程视角（统一挂载点 /merged）：
┌─────────────────────────────────┐
│ /merged/                        │
│   ├── bin/         ← lower层（squashfs只读）
│   ├── etc/
│   │   └── hosts    ← upper层（修改后的副本）
│   └── app/         ← upper层（新增文件）
└─────────────────────────────────┘

实际存储层次：
┌──────────────┐  ← upper dir（读写，XFS）
│ /upper/      │    容器运行时的写入目标
│   etc/hosts  │    （copy-up 后的修改）
│   app/       │
└──────────────┘
┌──────────────┐  ← work dir（overlayfs内部使用）
│ /work/       │    原子 copy-up 的临时目录
└──────────────┘
┌──────────────┐  ← lower dir（只读，squashfs）
│ /lower/      │    容器镜像层（可多层叠加）
│   bin/       │
│   etc/       │
│   etc/hosts  │    （原始文件，被upper覆盖）
└──────────────┘
```

## 9.2 挂载参数（参考 fs/overlayfs/super.c）

```bash
# 挂载命令
mount -t overlay overlay \
    -o lowerdir=/lower,upperdir=/upper,workdir=/work \
    /merged

# 多层 lower（容器镜像多层）：lowerdir=/layer3:/layer2:/layer1
# 最左侧优先级最高，最右侧优先级最低
```

```c
/* 参考 fs/overlayfs/super.c : ovl_fill_super() */
struct ovl_config {
    char        *lowerdir;   /* 冒号分隔的多层目录 */
    char        *upperdir;   /* 读写层（可为NULL，则整体只读）*/
    char        *workdir;    /* 工作目录（upper同一文件系统）*/
    bool         redirect_dir;
    bool         index;
    bool         metacopy;
};

/* overlayfs inode 私有数据 */
struct ovl_inode {
    struct inode *upper;     /* upper层对应的真实inode（可为NULL）*/
    struct inode *lower;     /* lower层对应的真实inode（可为NULL）*/
    struct inode *__upperdentry; /* 缓存 */
    const char   *redirect;  /* 重定向路径 */
};
```

## 9.3 路径查找：层次合并（参考 ovl_lookup）

```c
/* 参考 fs/overlayfs/inode.c : ovl_lookup() */
struct dentry *ovl_lookup(struct inode *dir, struct dentry *dentry,
                          unsigned int flags) {
    struct ovl_entry *oe;
    bool exists_upper = false, exists_lower = false;

    /* Step 1: 在 upper 层查找 */
    struct dentry *upper = lookup_one_len(dentry->d_name.name,
                                          ovl_upper_mnt(sb)->mnt_root,
                                          dentry->d_name.len);
    if (!IS_ERR(upper) && upper->d_inode) {
        /* 检查是否是 whiteout（删除标记）*/
        if (ovl_is_whiteout(upper)) {
            /* 文件已被删除，返回负dentry */
            return d_splice_alias(NULL, dentry);
        }
        exists_upper = true;
    }

    /* Step 2: 如果 upper 没有，在各 lower 层依次查找 */
    if (!exists_upper) {
        for (int i = 0; i < ovl_numlower(oe); i++) {
            struct dentry *lower = lookup_one_len(
                dentry->d_name.name,
                ovl_lower_mnt(sb, i)->mnt_root,
                dentry->d_name.len);
            if (!IS_ERR(lower) && lower->d_inode) {
                exists_lower = true;
                break;
            }
        }
    }

    /* Step 3: 建立 overlayfs dentry，记录来源层 */
    oe = ovl_alloc_entry(exists_lower ? 1 : 0);
    oe->has_upper = exists_upper;
    /* 将真实dentry保存到overlayfs dentry的私有数据 */
    ovl_set_upperpath(dentry, &upperpath);
    return d_splice_alias(new_inode, dentry);
}
```

## 9.4 copy-up机制（参考 fs/overlayfs/copy_up.c）

copy-up 是 overlayfs 写时复制的核心：当写入 lower 层的文件时，先将其复制到 upper 层。

```c
/* 参考 fs/overlayfs/copy_up.c : ovl_copy_up_one() */
int ovl_copy_up_one(struct dentry *parent, struct dentry *dentry,
                    int flags) {
    struct path lowerpath, upperpath;
    struct kstat stat;

    /* Step 1: 获取 lower 层文件信息 */
    ovl_path_lower(dentry, &lowerpath);
    vfs_getattr(&lowerpath, &stat, STATX_ALL, 0);

    /* Step 2: 在 work 目录创建临时文件（原子性保证）*/
    struct dentry *tmp = ovl_create_temp(workdir, &stat);

    /* Step 3: 复制文件内容 */
    if (S_ISREG(stat.mode)) {
        struct file *lower_file = ovl_path_open(&lowerpath, O_RDONLY);
        struct file *upper_file = ovl_path_open(&tmp_path, O_WRONLY);

        /* 按块复制（参考 copy_file_range）*/
        ovl_copy_file_range(lower_file, upper_file, stat.size);

        fput(lower_file);
        fput(upper_file);
    }

    /* Step 4: 复制元数据（权限、时间戳、xattr）*/
    ovl_copy_up_inode(dentry, tmp, &stat);

    /* Step 5: 原子 rename：work/tmp → upper/filename */
    ovl_do_rename(workdir, tmp, upperdir, upper_child, 0);

    /* Step 6: 更新 dentry 缓存，使后续操作走 upper */
    ovl_inode_update(dentry->d_inode, upper_child);
    return 0;
}
```

## 9.5 whiteout（文件删除标记）

```c
/* 参考 fs/overlayfs/dir.c */
/*
 * 在 overlayfs 中删除文件：
 * 1. 无法修改 lower 层（只读）
 * 2. 在 upper 层创建同名 whiteout 文件
 * 3. 路径查找时遇到 whiteout 就认为文件不存在
 */

/* whiteout 实现为字符设备文件，主次设备号 (0,0) */
#define WHITEOUT_MODE   (S_IFCHR | 0000)
#define WHITEOUT_MAJOR  0
#define WHITEOUT_MINOR  0

int ovl_unlink(struct inode *dir, struct dentry *dentry) {
    /* 如果文件在 upper：直接删除 */
    if (ovl_dentry_upper(dentry)) {
        return vfs_unlink(ovl_upper_mnt(dentry->d_sb)->mnt_root->d_inode,
                          ovl_dentry_upper(dentry), NULL);
    }

    /* 如果文件只在 lower：创建 whiteout */
    return ovl_create_whiteout(dir, dentry);
}

bool ovl_is_whiteout(struct dentry *dentry) {
    struct inode *inode = dentry->d_inode;
    return inode && IS_CHRDEV(inode) &&
           MAJOR(inode->i_rdev) == WHITEOUT_MAJOR &&
           MINOR(inode->i_rdev) == WHITEOUT_MINOR;
}
```

## 9.6 目录合并（readdir）

```c
/* 参考 fs/overlayfs/dir.c : ovl_iterate() */
/*
 * ls /merged/ 需要合并 upper + lower 的目录项
 * 规则：
 * 1. upper 的目录项优先（覆盖同名 lower 项）
 * 2. upper 中的 whiteout 屏蔽对应的 lower 项
 * 3. 去重（同名文件只显示一次）
 */
int ovl_iterate(struct file *file, struct dir_context *ctx) {
    struct ovl_dir_file *od = file->private_data;

    /* 先遍历 upper 层（如果存在）*/
    if (od->is_upper) {
        iterate_dir(od->realfile, &ovl_dir_iter_ctx);
    }

    /* 再遍历各 lower 层，跳过已在upper中出现的文件名 */
    for (int i = 0; i < ovl_numlower(oe); i++) {
        struct file *lower_file = ovl_path_open(&lower_path, O_RDONLY);
        ovl_iterate_lower(lower_file, ctx, &seen_names);
        fput(lower_file);
    }
    return 0;
}
```

## 9.7 完整容器存储链路

```
容器启动流程（结合 Phase 8 + Phase 9）：

1. 解压容器镜像 → squashfs（只读压缩层）
   mksquashfs /image_layer /image.sqsh -comp zstd

2. 挂载 squashfs 作为 lower 层
   mount -t squashfs /image.sqsh /lower -o ro

3. 准备 upper（XFS）和 work 目录
   mkdir -p /upper /work

4. 挂载 overlayfs
   mount -t overlay overlay \
       -o lowerdir=/lower,upperdir=/upper,workdir=/work \
       /merged

5. 容器进程在 /merged 中运行
   - 读操作：命中 upper（copy-up过的文件）或透传到 lower
   - 写操作：自动触发 copy-up，修改写入 upper
   - 删除操作：在 upper 创建 whiteout

6. 容器销毁：清空 upper 目录（lower squashfs 不变）
```

## 9.8 验证方法

```c
void test_overlayfs(void) {
    /* 挂载 squashfs 作为 lower */
    do_mount("/dev/vda", "/lower", "squashfs", MS_RDONLY, NULL);

    /* 挂载 overlayfs */
    char opts[] = "lowerdir=/lower,upperdir=/upper,workdir=/work";
    do_mount("overlay", "/merged", "overlay", 0, opts);

    /* 读取 lower 层文件（直接读，不触发 copy-up）*/
    int fd = sys_openat(AT_FDCWD, "/merged/etc/hosts", O_RDONLY, 0);
    printk("read lower file: OK (fd=%d)\n", fd);
    sys_close(fd);

    /* 写入文件（触发 copy-up）*/
    fd = sys_openat(AT_FDCWD, "/merged/etc/hosts", O_WRONLY, 0);
    sys_write(fd, "127.0.0.1 container\n", 20);
    sys_close(fd);

    /* 验证 upper 层有副本 */
    struct stat st;
    sys_stat("/upper/etc/hosts", &st);
    printk("copy-up OK: upper/etc/hosts size=%lld\n", st.st_size);

    /* 删除文件（创建 whiteout）*/
    sys_unlinkat(AT_FDCWD, "/merged/etc/resolv.conf", 0);
    /* 验证 whiteout 存在 */
    sys_lstat("/upper/etc/resolv.conf", &st);
    printk("whiteout OK: mode=0x%x\n", st.st_mode);
}
```

## 9.9 本阶段产出文件

```
arm64os/
└── fs/
    └── overlayfs/
        ├── super.c      ← 挂载与超级块（核心）
        ├── inode.c      ← inode操作（lookup、getattr）
        ├── dir.c        ← 目录合并、whiteout（核心）
        ├── file.c       ← 文件读写透传
        └── copy_up.c    ← copy-up机制（核心）
```
