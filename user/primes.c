#include "kernel/types.h"
#include "user/user.h"

void sub_pid(int r_pipe[2]){
        //0.读取管道的第一个元素是一个素数；如果没有则循环结束；
        //1.本线程：收取父进程的内容，并筛选，传递给子进程，直到读取结束；
        //1.创建子进程：收取当前进程筛选后的内容
        close(r_pipe[1]);//关闭写管道；
        int prime,sub_pipe[2],temp;
        int n=read(r_pipe[0],&prime,sizeof(int));
        if(n<0){//读取错误
            fprintf(2,"child-pid:%d read error",getpid());
            close(r_pipe[0]);
            exit(1);
        }
        else if(n==0){//正常退出
            close(r_pipe[0]);
            exit(0);
        }
        //父进程存在素数
        printf("prime %d\n",prime);//打印当前的素数
        pipe(sub_pipe);//创建管道
        int pid=fork();
        if(pid<0){//创建子进程错误
            fprintf(2,"fork error");
            close(sub_pipe[0]);
            close(sub_pipe[1]);
            close(r_pipe[0]);
            exit(1);
        }
        else if(pid==0){//子进程
            sub_pid(sub_pipe);
            exit(0);
        } 
        else{//当前进程
            close(sub_pipe[0]);//关闭读管道
            while(1){
                int n=read(r_pipe[0],&temp,sizeof(int));//从前置管道中读取内容
                if(n==0) break;//读取结束
                else if(n<0){//读取错误
                    fprintf(2,"child-pid:%d read error",getpid());
                    close(sub_pipe[1]);
                    close(r_pipe[0]);
                    exit(1);
                }
                else if(temp%prime!=0){//读取到了内容,判断并传递
                    if(write(sub_pipe[1],&temp,sizeof(int))!=sizeof(int)){//不能被当前素数整除，传递到下一层；
                        fprintf(2,"child-pid:%d write error",getpid());
                        close(sub_pipe[1]);
                        close(r_pipe[0]);
                        exit(1);
                    }
                }
            }
            close(sub_pipe[1]);
            close(r_pipe[0]);
            exit(0);            
        }
}

int main(){
    int p[2];
    pipe(p);//创建管道
    int pid=fork();
    if(pid<0){//创建子进程错误
        fprintf(2,"fork error");
        close(p[0]);
        close(p[1]);
        exit(1);
    }
    else if(pid==0){//子进程
        sub_pid(p);//关闭pipe要在子线程中执行；
        exit(0);
    }
    else{//父线程：将2-35传递给第一个子进程；
        close(p[0]);//关闭读管道
        for(int i=2;i<=35;++i){//主进程
            if(write(p[1],&i,sizeof(int))!=sizeof(int)){
                fprintf(2,"write error");
                close(p[1]);
                exit(0);
            }
        }
        close(p[1]);
        wait(0);
        exit(0);
    }
    return 0;
}