#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){

    if(argc < 2){
        printf("usage: xargs <command> [args]\n");
        exit(-1);
    }
    char* command = argv[1];
    char line[32];
    char* command_argv[32];

    command_argv[0]=command;
    for(int i = 2; i < argc; i++){
        command_argv[i-1]=argv[i];
    }

    while(1){
        gets(line, sizeof(line));
        if(line[0]==0)
            break;
        int len = strlen(line);
        if(line[len-1] == '\n' || line[len-1] == '\r')
            line[len-1]=0;
        command_argv[argc-1]=line;
        if(fork()==0){
            exec(command, command_argv);
            printf("exec\n");
        }
        wait(0);
    }

}