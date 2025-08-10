#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

void handler()
{
    printf("alarm!\n");
    sigreturn();
}

int main()
{
    sigalarm(2, handler);
    int c=0;
    for(int i =0; i < 1000000000; i++){
        c+=i;
    }
    printf("%d\n", c);
    return c;
}