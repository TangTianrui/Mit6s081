#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
#define O_NOFOLLOW 0x010//和其他的位不重复，应该是描述是否允许软连接的标识符

#define MAX_SYMLINK_DEPTH 10