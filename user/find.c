#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void find(const char* location, const char* filename){

    char loc[128];
    struct dirent de;
    struct stat st;

    strcpy(loc, location);
    int len = strlen(location);
    if(loc[len-1]!='/'){
        loc[len]='/';
        loc[++len]=0;
    }

    int fd = open(location, O_RDONLY);
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
        if(de.inum==0)
            continue;
        if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
        if(strcmp(filename, de.name)==0){
            printf("%s%s\n",loc,filename);
        }
        strcpy(loc+len, de.name);
        stat(loc, &st);
        if(st.type==T_DIR)
            find(loc, filename);
        loc[len]=0;
    }

}

int main(int argc, char* argv[]){

    char* location;
    char* filename;

    if(argc < 3){
        location=".";
        filename=argv[1];
    }else{
        location=argv[1];
        filename=argv[2];
    }

    find(location, filename);
}   