#ifndef XENON_COMPAT_SYS_IOCTL_H
#define XENON_COMPAT_SYS_IOCTL_H

#define FIONBIO  0x5421
#define FIONREAD 0x541B

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
