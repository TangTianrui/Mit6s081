#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char*
fmtname(char *path)
{
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

void
ls(char *path)
{
  char buf[512], *p;
  int fd;
  struct dirent de;//读取文件夹，得到具体条例信息
  struct stat st;//存储文件信息

  if((fd = open(path, 0)) < 0){//打开
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){//获取文件信息；
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE://如果目标文件是文件类型，打印信息
    printf("%s %d %d %l\n", fmtname(path), st.type, st.ino, st.size);//固定长度的文件名、文件类型、
    break;

  case T_DIR://如果目标文件是目录，
    //流程：
    //1.while(read fd->dirent;
    //2.fstat(path+dirent.name);
    //3.fmtname->printf;
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);//文件夹名添加到buf中
    p = buf+strlen(buf);//指针置于path后
    *p++ = '/';//由于是目录，添加‘/’
    while(read(fd, &de, sizeof(de)) == sizeof(de)){//循环读取目录中的文件信息；
      if(de.inum == 0)//inode=0；未使用的，或者是一个被删除后的占位项。
        continue;
      memmove(p, de.name, DIRSIZ);//将文件信息中的文件名添加到目录中；
      p[DIRSIZ] = 0;//结束符
      if(stat(buf, &st) < 0){//没有正确读取到文件stat信息；
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, st.size);//根据文件名获取的stat信息输出；
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
