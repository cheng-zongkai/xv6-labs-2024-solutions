#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {

    int ppid = getpid();
    int pid;
    int pipdfds[2];
    char data='A';

    if(pipe(pipdfds)==-1){
        printf("pipe\n");
        return -1;
    }

    if((pid=fork())==0){
        int cpid = getpid();
        int n = read(pipdfds[0], &data, 1);
        if(n==-1){
            printf("child read\n");
        }else if (n==0){
            printf("parent closed the pipe prematurely\n");
        }
        printf("%d: received ping\n", cpid);
        if(write(pipdfds[1], &data, 1) == -1){
            printf("child write\n");
            return -1;
        }
    }else{
        if(write(pipdfds[1], &data, 1) == -1){
            printf("parent write\n");
            return -1;
        }
        wait(0);
        int n = read(pipdfds[0], &data, 1);
        if(n==-1){
            printf("parent read\n");
        }else if (n==0){
            printf("child closed the pipe prematurely\n");
        }
        printf("%d: received pong\n", ppid);
    }

    return 0;
}