#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int pipe_p[2] = {-1,-1};
int pipe_c[2] = {-1,-1};

void __attribute__((noreturn)) primes () {
    int mypid=getpid();
    int p, n;
    read(pipe_p[0], &p, sizeof(int)); /* 父进程已经设置好pipe_p[0] */
    printf("prime %d\n", p);
    while(read(pipe_p[0], &n, sizeof(int))>0){
        if(n % p != 0){
            if(pipe_c[1]==-1){
                int pid;
                if(pipe(pipe_c)==-1){
                    printf("%d: pipe error\n", mypid);
                    exit(-1);
                }
                if((pid=fork())==0){
                    close(pipe_c[1]);
                    close(pipe_p[0]);
                    pipe_p[0]=pipe_c[0];
                    pipe_p[1]=-1;
                    pipe_c[1]=pipe_c[0]=-1;

                    primes();
                }else if(pid==-1){
                    printf("fork error\n");
                }else{
                    close(pipe_c[0]);
                    pipe_c[0]=-1; 
                }
            }
            write(pipe_c[1], &n, sizeof(int));
        }
    }
    close(pipe_c[1]);
    wait(0);
    exit(0);
}

int main(){

    pipe(pipe_c);

    if(fork()==0){
        close(pipe_c[1]);
        
        pipe_p[0]=pipe_c[0];
        pipe_p[1]=-1;
        pipe_c[1]=pipe_c[0]=-1;

        primes();
    }else{
        close(pipe_c[0]);
        pipe_c[0]=-1; 
    } 

    for(int i = 2; i <= 280; i++){
        write(pipe_c[1], &i, sizeof(int));
    }

    close(pipe_c[1]);
    wait(0);
    return 0;
}