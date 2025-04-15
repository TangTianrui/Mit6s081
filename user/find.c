#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

/*
char* fmtname(char *path){//实现路径名的固定长表示
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;//反向找到最底层的文件名；

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)//如果p长度超过最大文件名长度，直接返回文件名头指针，不利用buf填充空；
    return p;
  memmove(buf, p, strlen(p));//先把文件名填充到buf中，
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));//没有达到DIRSIZ，填充“ ”(空)
  return buf;
}
*/

char *tar_fn;

void find_dir(char *path){
    //递归实现对目录的搜索
    char buf[512],*p;//临时的文件路径和路径操作指针；
    int fd;//打开的目录文件描述符
    struct dirent de;//用于遍历目录时存储文件概要信息
    struct stat st;//用于存储文件详细信息
    if((fd=open(path,0))<0){//以只读打开
        fprintf(2,"dir open faild");
        exit(1);
    }
    if(fstat(fd,&st)<0){//获取文件详细信息
        fprintf(2,"dir fstat get failed");
        close(fd);
        exit(1);
    }
    if(st.type!=T_DIR){//如果不是文件夹，则报错
        fprintf(2,"path is not a dir");
        close(fd);
        exit(1);
    }
    strcpy(buf,path);
    p=buf+strlen(path);
    *p++='/';//buf=path+'/';
    while(read(fd,&de,sizeof(struct dirent))==sizeof(struct dirent)){//文件夹中读取到了文件概要信息
        if(de.inum==0) continue;
        memmove(p,de.name,DIRSIZ);//buf=path+'/'+'filename';
        p[DIRSIZ]=0;//buf=path+'/'+'filename'+'\0';字符串结束符
        if(stat(buf,&st)<0){//获取文件信息失败
            printf("filename:%s stat get failed",buf);
            continue;
        }
        switch (st.type){
            case T_DIR: //递归find_dir;
                if(strcmp(".",de.name)!=0&&strcmp("..",de.name)!=0) find_dir(buf);//目录名不为“.”和“..”则递归find
                break;
            case T_FILE: //判断文件名是否和目标文件名相同
                if(strcmp(tar_fn,de.name)==0){
                    printf("%s\n",buf);
                }
                break;
            default:
                break;
        }
    }
    close(fd);
    return;
}

int main(int argc, char *argv[])
{
  if(argc != 3){//
    fprintf(2,"error input! you should input like:find <path> <filename>\n");
    exit(1);
  }
  tar_fn=argv[2];//目标文件名
  find_dir(argv[1]);
  exit(0);
}