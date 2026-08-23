#ifndef STUB_NET_H
#define STUB_NET_H
#include <psp2/types.h>
#define SCE_NET_AF_INET 2
#define SCE_NET_SOCK_STREAM 1
#define SCE_NET_SOL_SOCKET 0xffff
#define SCE_NET_SO_RCVBUF 0x1002
#define SCE_NET_SO_RCVTIMEO 0x1006
#define SCE_NET_SO_SNDTIMEO 0x1005
#define SCE_NET_ERROR_ENOTINIT -2143223803
typedef struct { unsigned int s_addr; } SceNetInAddr;
typedef struct { unsigned char sin_len, sin_family; unsigned short sin_port;
                 SceNetInAddr sin_addr; char sin_zero[8]; } SceNetSockaddrIn;
typedef struct { unsigned char sa_len, sa_family; char sa_data[14]; } SceNetSockaddr;
typedef struct { void *memory; int size; int flags; } SceNetInitParam;
int sceNetInit(SceNetInitParam*);
int sceNetShowNetstat(void);
int sceNetSocket(const char*,int,int,int);
int sceNetConnect(int,SceNetSockaddr*,unsigned int);
int sceNetSend(int,const void*,unsigned int,int);
int sceNetRecv(int,void*,unsigned int,int);
int sceNetSocketClose(int);
int sceNetSetsockopt(int,int,int,const void*,unsigned int);
int sceNetInetPton(int,const char*,void*);
unsigned short sceNetHtons(unsigned short);
#endif
