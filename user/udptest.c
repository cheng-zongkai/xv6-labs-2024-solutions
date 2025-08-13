#ifndef LAB_NET
#define LAB_NET
#endif

#include "kernel/types.h"
#include "kernel/net.h"
#include "kernel/stat.h"
#include "user/user.h"

int main()
{
    char buf[256];
    uint32 srcaddr;
    uint16 sport;
    bind(2000);
    while(1){
        int n = recv(2000, &srcaddr, &sport, buf, 256);
        printf("recv: ip: %x sport: %d len: %d\n%s\n",srcaddr,sport,n,buf);
    }
    unbind(2000);
}