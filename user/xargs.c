#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"//文件信息
#include "user/user.h"//用户
#include "kernel/fs.h"//文件系统

#define MSGSIZE 16

int main(int argc,char *argv[]){
    //读取命令行的内容，运行命令行传入的指令；
    //1.获取xargs后面的参数->直接通过argc和argv可以得到；
    //2.获取管道符“|”传递的参数->由于管道符的输出写入标准输入，则通过read标准输出可以得到；
    //3.通过fork和exec分离子进程，并执行参数列表对应的命令；
    
    //2.获取管道符传递的参数
    char buf[MSGSIZE];
    if(read(0,buf,MSGSIZE)<0){//通过读标准输入，获取管道的输入，如果为0说明没有；如果<0说明读取错误；
        fprintf(2,"read msg error");
        exit(1);
    }
    //printf("printf readed: %s",buf);    
    //1.将自己的参数列表重新整理
    char *xargv[MAXARG];//定义xargs参数数组和最大参数数量；
    int xargc=0;//定义当前的参数数量；
    for(int i=1;i<argc;++i){//由于argv[0]="xargs",代表调用本函数的功能，所以实现时不用考虑；
        xargv[xargc++]=argv[i];
    }
    //3.根据自己的参数列表和管道符执行命令行命令；
    //3.1.根据已有的参数列表，从标准输入中逐字符读取输入，如果遇到'\n'，则执行一次，否则全是添加参数；
    char *p=buf;//指针标定buf的起点；
    for(int i=0;i<MSGSIZE;++i){
        if(buf[i]=='\n'){//如果是换行符，则需要进行一次执行
            int pid=fork();//创建子进程，用于执行任务；父进程继续buf的读取；
            if(pid<0){//子进程创建错误
                fprintf(2,"pid fork error");
                exit(1);
            }
            if(pid==0){//子进程通过exec执行命令
                //printf("printf: %s",p); 
                buf[i]=0;//手动将'\n'设置为字符串的终点
                xargv[xargc++]=p;//将目前读取到的一段字符串置为末尾的参数；
                xargv[xargc]=0;//将末尾参数置为空，标志参数的结束；
                exec(xargv[0],xargv);//执行对应命令；
                exit(0);
            }
            else{//父进程
                p=&buf[i+1];//手动将下一个参数的起点置于'\n'的后面；
                //wait(0);//等待子进程执行结束；(需要吗，父进程可以同步执行，通过并发的子进程实现多次命令?)
            }
        }
    }
    //wait(0);//可能存在问题，既for循环中父进程可能创建了多个子进程执行exec,此处wait只会回收一个子进程，其他进程成为了僵尸进程
    while(wait(0)>0);//通过while循环回收所有子进程的资源，避免僵尸进程
    //wait(0);//意义不明
    exit(0);
    return 0;
}