#include "kernel/types.h"
#include "user/user.h"

int main(){
    int p1[2],p2[2];
    pipe(p1);//创建管道
    pipe(p2);
    int pid=fork();
    //char buf='a';
    if(pid>0){
        //父进程0读，1写；
        close(p1[0]);
        close(p2[1]);
        if(write(p1[1],(int*)0,sizeof(int))!=sizeof(int)){//发送1个字节；
            fprintf(2,"parent write error");
            close(p1[1]);
            close(p2[0]);
            exit(0);
        }
        if(read(p2[0],(int*)0,sizeof(int))!=sizeof(int)){//读取1个字节；
            fprintf(2,"parent read error");
            close(p1[1]);
            close(p2[0]);
            exit(0);           
        }
        else{
            printf("%d: received pong\n",getpid());             
        }
        close(p1[1]);
        close(p2[0]);
        wait(0);
        exit(0);
    }
    else if(pid==0){
        //子进程1读，0写；
        close(p1[1]);
        close(p2[0]);
        if(read(p1[0],(int*)0,sizeof(int))!=sizeof(int)){//读取1个字节；
            fprintf(2,"child read error");
            close(p1[1]);
            close(p2[0]);
            exit(0);
        }
        else{
            printf("%d: received ping\n",getpid());            
        }
        if(write(p2[1],(int*)1,sizeof(int))!=sizeof(int)){//发送1个字节；
            fprintf(2,"child write error");   
        }
        close(p1[1]);
        close(p2[0]);
        exit(0);     
    }
    else {//错误；
        fprintf(2,"fork error");
        close(p1[0]);
        close(p2[0]);
        close(p1[1]);
        close(p2[1]);
        exit(0);
    }
    return 0;
}


