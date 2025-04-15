#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device

struct stat {
  int dev;     // File system's disk device//文件所在硬盘的编号
  uint ino;    // Inode number//文件ind结点号，唯一标识文件或目录；
  short type;  // Type of file//文件类型；
  short nlink; // Number of links to file//有多少个硬链接指向它，
  uint64 size; // Size of file in bytes//文件大小bytes
};
