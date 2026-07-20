#pragma  once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**********************************公用系统文件集合********************************/
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <float.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h> //NDEBUG to disable.
#include <signal.h>
#include <stdbool.h>
#if ( __STDC_VERSION__ >= 201112L ) 
#include <stdnoreturn.h>
#include <stdatomic.h>
#include <stdalign.h>
//#include <threads.h>/
#include <uchar.h>
#include <tgmath.h>
#endif
#ifdef  WIN32
#include <windows.h>
#include <corecrt_io.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <corecrt_io.h>
#include <wchar.h>
#include <process.h>
#else
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/timeb.h> 
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h> 
#include <netinet/udp.h> 
#include <sys/resource.h>
#include <execinfo.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <grp.h>
#include <pwd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <iconv.h>
#include <libgen.h>
/* Prefer libc gettid(2) when available; avoid syscall() which needs
 * _GNU_SOURCE before the first unistd.h include (often too late here). */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 30)
/* gettid() provided by unistd.h */
#elif defined(SYS_gettid)
static inline pid_t sunpf_gettid(void)
{
    /* Explicit prototype: may be undeclared without early _GNU_SOURCE. */
    extern long syscall(long number, ...);
    return (pid_t)syscall(SYS_gettid);
}
#define gettid() sunpf_gettid()
#else
#define gettid() ((pid_t)getpid())
#endif

#endif
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifdef __cplusplus
#include <iostream>
#include <future>
//#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <queue>
#include <stack>
#include <list>
#include <tuple>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#endif

#include "yyjson.h"
#include "uthash.h"

