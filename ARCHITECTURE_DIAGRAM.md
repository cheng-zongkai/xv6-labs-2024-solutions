# xv6 MMAP实现架构图

## 系统调用流程

```
用户程序
   |
   | mmap(addr, len, prot, flags, fd, offset)
   |
   v
sys_mmap() [kernel/mmap.c]
   |
   |-- 1. 验证参数 (len!=0, fd有效, 权限检查)
   |-- 2. 查找空闲VMA槽 (遍历p->vma数组)
   |-- 3. 计算VMA地址 (从TRAPFRAME向下分配)
   |-- 4. 增加文件引用 filedup(f)
   |-- 5. 初始化VMA结构
   |-- 6. 返回虚拟地址
   |
   v
返回给用户 (不分配物理内存)
```

## 页面错误处理流程

```
用户访问mmap区域
   |
   | 触发Page Fault (0xd=读, 0xf=写)
   |
   v
usertrap() [kernel/trap.c:73-75]
   |
   | r_scause() == 0xd || 0xf
   |
   v
mmaphandler() [kernel/trap.c:227-291]
   |
   |-- 1. 获取故障地址 va = r_stval()
   |-- 2. 查找对应VMA (遍历p->vma数组)
   |-- 3. 验证权限 (PROT_READ/WRITE)
   |-- 4. 计算文件偏移 offset = vma->offset + (va - vma->addr)
   |-- 5. 分配物理页 pa = kalloc()
   |-- 6. 读取文件内容 readi(ip, pa, offset, len)
   |-- 7. 建立页表映射 walk() + 设置PTE
   |
   v
返回用户空间继续执行
```

## munmap流程

```
用户程序
   |
   | munmap(addr, len)
   |
   v
sys_munmap() [kernel/mmap.c:71-79]
   |
   v
munmap() [kernel/mmap.c:81-151]
   |
   |-- 1. 查找对应VMA
   |-- 2. 遍历要释放的页面
   |       |
   |       |-- 检查页面是否已分配 (PTE_V)
   |       |-- MAP_SHARED且脏页 -> 写回文件
   |       |   |
   |       |   |-- begin_op()
   |       |   |-- writei(ip, pa, offset, n)
   |       |   |-- end_op()
   |       |
   |       |-- uvmunmap() 释放物理内存
   |
   |-- 3. 更新VMA (缩小或删除)
   |-- 4. 如果VMA完全释放 -> fileclose(f)
   |
   v
返回0表示成功
```

## fork时的VMA处理

```
fork() [kernel/proc.c:282-336]
   |
   |-- 分配子进程
   |-- 复制用户内存
   |-- 复制文件描述符
   |
   |-- 复制VMAs [313-319]:
   |       for each parent VMA:
   |         |-- 复制VMA结构
   |         |-- filedup(vma->f) 增加引用
   |         |-- 子进程获得相同映射
   |
   |-- 复制其他状态
   |
   v
返回子进程PID
```

## exit时的VMA清理

```
exit() [kernel/proc.c:357-404]
   |
   |-- 清理所有VMAs [365-370]:
   |       for each VMA:
   |         |-- munmap(vma->addr, vma->len)
   |               |-- 写回脏页
   |               |-- 释放物理内存
   |               |-- 关闭文件
   |
   |-- 关闭文件描述符
   |-- 释放其他资源
   |
   v
进程变为ZOMBIE状态
```

## 数据结构关系

```
struct proc {
    ...
    struct vma vma[16];  // 每个进程最多16个VMA
    ...
}
    |
    |-- vma[0]: struct vma
    |       |-- addr: 0x3000
    |       |-- len: 0x5000
    |       |-- prot: PROT_READ|PROT_WRITE
    |       |-- flags: MAP_SHARED
    |       |-- f: struct file* -----> struct file {
    |       |-- offset: 0x1000         |-- ref: 2
    |                                   |-- ip: struct inode*
    |-- vma[1]: struct vma              |-- ...
    |       ...                         }
    |
    |-- vma[15]: struct vma
```

## 关键常量和宏

```c
NELEM(p->vma)  = 16        // 最大VMA数量
TRAPFRAME      = 高地址     // VMA分配起始点
PGSIZE         = 4096      // 页面大小

// 权限位
PROT_READ      = 0x1
PROT_WRITE     = 0x2
PROT_EXEC      = 0x4

// 映射标志
MAP_SHARED     = 0x01
MAP_PRIVATE    = 0x02

// 页表项标志
PTE_V          = 0x001    // 有效位
PTE_R          = 0x002    // 可读
PTE_W          = 0x004    // 可写
PTE_X          = 0x008    // 可执行
PTE_U          = 0x010    // 用户模式
PTE_D          = 0x080    // 脏位
```

## 文件布局

```
kernel/
├── mmap.c          [151行] - mmap/munmap系统调用实现
│   ├── sys_mmap()       : mmap系统调用入口
│   ├── sys_munmap()     : munmap系统调用入口
│   └── munmap()         : munmap核心逻辑
│
├── trap.c          [291行] - 页面错误处理
│   ├── usertrap()       : 用户陷阱处理 (调用mmaphandler)
│   └── mmaphandler()    : MMAP页面错误处理器
│
├── proc.c          [713行] - 进程管理
│   ├── allocproc()      : 初始化VMA数组
│   ├── fork()           : 复制父进程VMAs
│   └── exit()           : 清理进程VMAs
│
├── proc.h          - 数据结构定义
│   ├── struct vma       : VMA结构定义
│   └── struct proc      : 包含vma[16]数组
│
└── vm.c            [451行] - 虚拟内存管理
    └── (使用walk, mappages等辅助函数)

user/
└── mmaptest.c      - 测试程序
```

## 执行时间线示例

```
时间线: mmap一个文件并访问

T0: 用户调用 mmap(0, 8192, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
    └─> sys_mmap分配VMA在地址0x3000，len=8192
    └─> 返回0x3000 (未分配物理内存)

T1: 用户访问 addr[0] = 'A' (写入第一个字节)
    └─> 触发Page Fault (scause=0xf)
    └─> mmaphandler():
        ├─ va=0x3000, offset=0
        ├─ kalloc() 分配物理页
        ├─ readi() 读取文件[0-4096]
        ├─ 建立页表映射 0x3000 -> PA
        └─ 设置PTE_W|PTE_R|PTE_U|PTE_V
    └─> 用户继续执行，写入成功

T2: 用户访问 addr[5000] (第二个页面)
    └─> 触发Page Fault (scause=0xf)
    └─> mmaphandler():
        ├─ va=0x4000 (PGROUNDDOWN)
        ├─ offset=4096
        ├─ kalloc() 分配第二个物理页
        ├─ readi() 读取文件[4096-8192]
        └─> 建立映射
    └─> 用户继续执行

T3: 用户调用 munmap(0x3000, 4096)
    └─> munmap():
        ├─ 检查脏位PTE_D (已设置)
        ├─ writei() 写回第一页到文件
        ├─ uvmunmap() 释放物理内存
        └─ 更新VMA: addr=0x4000, len=4096

T4: 用户调用 munmap(0x4000, 4096)
    └─> munmap():
        ├─ 写回第二页 (如果脏)
        ├─ 释放物理内存
        ├─ VMA len变为0
        └─> fileclose(f) 关闭文件
```

## 性能特点

**优点:**
- ✅ 懒加载: 节省内存，只在需要时分配
- ✅ 脏页追踪: 只写回修改的页面
- ✅ 引用计数: 多进程可安全共享映射

**限制:**
- ⚠️ VMA数量: 最多16个
- ⚠️ 线性搜索: O(n)查找VMA
- ⚠️ 单页I/O: 没有预读或批量写回

## 关键设计决策

1. **自上而下分配地址空间**
   - 从TRAPFRAME开始向下分配
   - 简单但可能导致碎片

2. **固定大小VMA数组**
   - 简单实现
   - 有数量限制

3. **每次一页的I/O**
   - 实现简单
   - 可能影响性能

4. **在munmap时写回**
   - 保证数据一致性
   - 可能造成延迟
