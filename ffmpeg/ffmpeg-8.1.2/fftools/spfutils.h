
/*******************************************************************************
 * Copyright (C), 1990-2020, Tech.Co., Ltd. All rights reserved.
 * @file   : utils.h
 * @author : sunpf
 * @mail   : perfectsun1990@163.com 
 * @version: v1.0.0
 * @date   : 2018年05月20日 星期二 11时57分04秒 
 *-----------------------------------------------------------------------
 * @detail : 公共模块
 * @         功能:系统接口、全局变量、宏及控制/通信协议.
 *******************************************************************************/

#ifndef __SUNPF_UTIL__
#define __SUNPF_UTIL__
/*******************************************************************************/
/**Note: Enable/Disable module by your needs.*/
#define __USE_HEADER__  1         /**Note: public headers, just for rapid devs.*/
#define __USE_DBGAPI__  1         /**Note: public debug apis, logger framework.*/
#define __USE_COMMON__  1         /**Note: common tool micros and bits options.*/
#define __USE_CTYPES__  1         /**Note: extend C types and configer options.*/
#define __USE_THREAD__  1         /**Note: multiple process or threads options.*/
#define __USE_SOCKET__  1         /**Note: extend 0mq network & socket options.*/
#define __USE_STRING__  1         /**Note: extend and advanced strings options.*/
#define __USE_TIMMER__  1         /**Note: high-precision and fmt time options.*/
#define __USE_SYSTEM__  1         /**Note: system limits or systime(r) options.*/
/*******************************************************************************/
/**Note: public headers, just for rapid devs.*/
#ifdef __USE_HEADER__
#include "header.h" 
#endif
/*******************************************************************************/
/**Note: public debug apis, logger framework.*/
#ifdef __USE_DBGAPI__
#define _red "\e[0;32;31m" 
#define _yel "\e[0;32;33m" 
#define _blu "\e[0;32;34m" 
#define _rst "\e[0m"
typedef enum { LOG_ERR, LOG_WAR, LOG_MSG, LOG_DBG, } LogLev;
typedef void (*LogPtr)(int32_t level, const char *fmt, ...);
typedef struct Ilog { LogPtr ptr; LogLev lev; } Ilog;
static inline  Ilog* get_logger(void) {static Ilog cb={NULL, LOG_MSG}; return &cb;}
static inline void   set_logger(LogPtr out_ptr, LogLev out_lev) {
    Ilog* p_cb = get_logger(); p_cb->ptr = out_ptr; p_cb->lev = out_lev;
}
#define Logger ({ Ilog *p_cb = get_logger();        p_cb->ptr; })
#define Loglev ({ Ilog *p_cb = get_logger();        p_cb->lev; })
static inline void def_logger(int32_t level, char * format,  ...) {
    if (level > Loglev || level < 0)
        return;
    char buffer[1024] = {0};
    va_list ap;
    va_start(ap, format);
    vsprintf(buffer, format, ap);
    va_end(ap);
    fputs(buffer, stdout); //实际使用：写文件或调用其他日志接口即可...
}
static inline char* get_fmttime(void);
#define __NOW__     get_fmttime()
#define FFMPEG_APIS 1
#ifndef FFMPEG_APIS
//Note: Just use for debug, it's better to use other log system!
#define _dbg(fmt, ...)  do { if( LOG_DBG <= Loglev )                            \
    printf(_rst "[%s:<%s>:%d:D] " fmt _rst, __NOW__, __FUNCTION__, __LINE__,    \
    ##__VA_ARGS__ );}while(0)    
#define _msg(fmt, ...)  do { if( LOG_MSG <= Loglev )                            \
    printf(_blu "[%s:<%s>:%d:I] " fmt _rst, __NOW__, __FUNCTION__, __LINE__,    \
    ##__VA_ARGS__ );}while(0)    
#define _war(fmt, ...)  do { if( LOG_WAR <= Loglev )                            \
    printf(_yel "[%s:<%s>:%d:W] " fmt _rst, __NOW__, __FUNCTION__, __LINE__,    \
    ##__VA_ARGS__ );}while(0)    
#define _err(fmt, ...)  do { if( LOG_ERR <= Loglev )                            \
    printf(_red "[%s:<%s>:%d:E] " fmt _rst, __NOW__, __FUNCTION__, __LINE__,    \
    ##__VA_ARGS__ );}while(0)
#define _prt(fmt, ...)  do {                                                    \
    printf(_rst "[%s:<%s>:%d:=] " fmt _rst, __NOW__, __FUNCTION__, __LINE__,    \
    ##__VA_ARGS__ );}while(0)    
#else
#include "libavutil/log.h"
#define _dbg( fmt, ... )do { if( LOG_DBG <= Loglev )                            \
    av_log(NULL, AV_LOG_DEBUG, "[%s:<%s>:%d:D] " fmt, __NOW__, __FUNCTION__,    \
    __LINE__, ##__VA_ARGS__);}while(0)        
#define _msg( fmt, ... )do { if( LOG_MSG <= Loglev )                            \
    av_log(NULL, AV_LOG_INFO,  "[%s:<%s>:%d:I] " fmt, __NOW__, __FUNCTION__,    \
    __LINE__, ##__VA_ARGS__);}while(0)        
#define _war( fmt, ... )do { if( LOG_WAR <= Loglev )                            \
    av_log(NULL,AV_LOG_WARNING,"[%s:<%s>:%d:W] " fmt, __NOW__, __FUNCTION__,    \
    __LINE__, ##__VA_ARGS__);}while(0)        
#define _err( fmt, ... )do { if( LOG_ERR <= Loglev )                            \
    av_log(NULL, AV_LOG_ERROR, "[%s:<%s>:%d:E] " fmt, __NOW__, __FUNCTION__,    \
    __LINE__, ##__VA_ARGS__);}while(0)
#define _prt( fmt, ... )do { if( LOG_ERR <= Loglev )                            \
    av_log(NULL, AV_LOG_FATAL, "[%s:<%s>:%d:=] " fmt, __NOW__, __FUNCTION__,    \
    __LINE__, ##__VA_ARGS__);}while(0)        
#endif
#define dbg(fmt, arg...)  do { if( Logger != NULL ) {                           \
    Logger(LOG_DBG, "[%s:<%s>:%d:D] " fmt , __NOW__, __FUNCTION__, __LINE__,    \
    ## arg ); } else{ _dbg(fmt, ## arg);}} while(0)
#define msg(fmt, arg...)  do { if( Logger != NULL ) {                           \
    Logger(LOG_MSG, "[%s:<%s>:%d:I] " fmt , __NOW__, __FUNCTION__, __LINE__,    \
    ## arg ); } else{ _msg(fmt, ## arg);}} while(0)
#define war(fmt, arg...)  do { if( Logger != NULL ) {                           \
    Logger(LOG_WAR, "[%s:<%s>:%d:W] " fmt , __NOW__, __FUNCTION__, __LINE__,    \
    ## arg ); } else{ _war(fmt, ## arg);}} while(0)
#define err(fmt, arg...)  do { if( Logger != NULL ) {                           \
    Logger(LOG_ERR, "[%s:<%s>:%d:E] " fmt , __NOW__, __FUNCTION__, __LINE__,    \
    ## arg ); } else{ _err(fmt, ## arg);}} while(0)
#define prt(fmt, arg...)  do { if( Logger != NULL ) {                           \
    Logger(LOG_ERR, "[%s:<%s>:%d:=] " fmt , __NOW__, __FUNCTION__, __LINE__,    \
    ## arg ); } else{ _prt(fmt, ## arg);}} while(0)
#define out(fmt, ...)   fprintf(stderr, fmt ,  ##__VA_ARGS__ )
#if FFMPEG_APIS
/* after err/msg macros so utils_stream can use them */
#include "utils_stream.h"
#endif

// 查看函数及行号: addr2line -e ./backtrace -f 0x804865f 
static void print_stack(void);
static inline char* get_struuid(const char* str);
#define Assertor(x) do { if(!(x)) {                                             \
    err("assert '<%s>' failed, aborted!\n", #x); print_stack(); abort();}}while(0)
#define TryCatch(x, fmt, ...) do { if((x)) {                                    \
    err("except '<%s>' occured, failed!\n"fmt"\n",#x, ##__VA_ARGS__ );          \
        goto Exception;}} while(0)
#endif//__USE_DBGAPI__
/*******************************************************************************/
/**Note: common tool micros and bits options.*/
#ifdef __USE_COMMON__
#define BIT_SET(x,n)        ( (x) |=  ( 1 << (n) ) )     // 设置x的第n位为"1"
#define BIT_CLR(x,n)        ( (x) &= ~( 1 << (n) ) )     // 清除x的第n位为"0"
#define BIT_CHK(x,n)        ( ( (x) >> (n) ) & 1 )       // 探测某位是否是"1"

#define BCUT_04(x,n)        ( ( (x) >> (n) ) & 0x0F    ) // 获取x的(n~n+03)位
#define BCUT_08(x,n)        ( ( (x) >> (n) ) & 0xFF    ) // 获取x的(n~n+07)位
#define BCUT_16(x,n)        ( ( (x) >> (n) ) & 0xFFFF  ) // 获取x的(n~n+15)位
#define BCUT_24(x,n)        ( ( (x) >> (n) ) & 0x3FFFF ) // 获取x的(n~n+23)位

//#define _LITTLE           ( __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ )
#define SWAP_16(x)          ((x>>08&0xff)|(x<<08&0xff00))// 大小端字节序B转换
#define SWAP_24(x)          ((x>>16&0xff)|(x<<16&0xff0000)|x&0xff00)  
#define SWAP_32(x)          ((x>>24&0xff)|(x>>8&0xff00) \
                                |(x << 8 & 0xff0000) | (x << 24 & 0xff000000))
#define COUNTS(s)            ( sizeof(s)/sizeof(s[0]) )
#define _S(x)    #x
#define STR(x)  _S(x)
//
#define IS_POW_OF_2(x)                  ((x) > 0 && (((x) & ((x) - 1)) == 0))

#define max2(a, b) ((a) > (b) ? (a) : (b))
#define min2(a, b) ((a) < (b) ? (a) : (b))

#define max3(a, b, c) max2(max2((a), (b)), (c))
#define min3(a, b, c) min2(min2((a), (b)), (c))
/**
 *@brief 获取最接近value的2的幂.
 */
static inline uint32_t
get_pow2val(uint32_t value) {
    if (!IS_POW_OF_2(value)) {
        uint32_t p2val = 1;
        while(p2val < value)
            p2val    <<= 1;
        return p2val;
    }// ref: find the min p2val >value.
    return value;
}

#define dedup_num_arr(arr_, num_) ({ int32_t k = 1;                             \
    if (num_ > 1) {                                                             \
        for (int32_t i = 1; i < num_; ++i) {                                    \
            while (i < num_ && arr_[i] == arr_[i - 1])                          \
                i++;                                                            \
            if (i < num_)                                                       \
                arr_[k++] = arr_[i];                                            \
            else                                                                \
                break;                                                          \
        }                                                                       \
    } k;})
#define dedup_ptr_arr(arr_, num_) ({ int32_t k = num_;                          \
    if (num_ > 1) {                                                             \
        for (int32_t i = 0; i < num_; ++i) {                                    \
            for (int32_t j = i+1; j < num_; ++j) {                              \
                if (arr_[i] && arr_[j]                                          \
                    && arr_[i] == arr_[j]) {                                    \
                    arr_[j] = NULL;                                             \
                }                                                               \
    }}}k;})
#endif//__USE_COMMON__
/*******************************************************************************/
/**Note: Extend C types and configer options.*/
#ifdef __USE_CTYPES__
// classlike interface.
#ifndef CLASS
#define CLASS(_tp_name) typedef struct _tp_name _tp_name; struct _tp_name
#endif
#ifndef CSync
#define CSync                 CSync
typedef enum     CSync {
    SY_NO, //无锁(1)
    SY_MU, SY_MU_SHARED,    //互斥(2)
    SY_MR, SY_MR_SHARED,    //递归(2)
    SY_SP, SY_SP_SHARED,    //自旋(2)
    SY_RW, SY_RW_SHARED,    //读写(2)
    SY_SM, SY_SM_SHARED,    //信号(2)
    SY_CO, SY_CO_SHARED,    //条件(2)
    SY_UP, //上限(1)
} CSync;
#endif
/**Note: Describe component status|attributes.*/
typedef enum     CStat {
    E_INVALID           = 0x00000000,
    E_INITBEG, E_INITEND,
    E_STRTING, E_STARTED,
    E_RUNNING, E_PAUSING,
    E_STOPING, E_STOPPED,
    E_DELTING, E_DELTEND,
    E_INITERR, E_STRTERR, E_RUNNERR,
    E_PAUSERR, E_STOPERR, E_DELTERR, 
    E_UNKNOWN           = 0xFFFFFFFF,
} CStat,CStatus;//Ext...
typedef enum     CAttr {
    P_INVALID                   = 0,
    P_BEGIN, P_SEEKD, P_PAUSE, P_ENDED,
    P_VIDEO, P_AUDIO, P_UDATA,
    //...
    P_UNKNOWN   = sizeof(int64_t)*8,
} CAttr;
#define chk_attr(x,y)   BIT_CHK(x,y)
#define set_attr(x,y)   BIT_SET(x,y)
#define clr_attr(x,y)   BIT_CLR(x,y)
#define rst_attr(x)        ( x = 0 )
typedef union    CAuto {
    int8_t  i08; uint8_t  u08; int16_t i16; uint16_t u16; 
    int32_t i32; uint32_t u32; int64_t i64; uint64_t u64;
    double  dbl; void*    ptr; char*   str;
} CAuto;
typedef uint8_t CType;
//------------------------------------------------------------------------------
#define CTAGS_MASK ((uint8_t)0xFF)      /* 11111111 */
#define CTAGS_BITS ((uint8_t)8)         /* 00001000 */
#define CTYPE_MASK ((uint8_t)0x0F)      /* 00001111 */
#define CTYPE_BITS ((uint8_t)4)         /* 00000100 */
//------------------------------------------------------------------------------
#define ARRPTR     ((uint8_t)(1  << 4)) /* 0001____ */
#define SINT08     ((uint8_t)(0  << 0)) /* ____0000 */
#define UINT08     ((uint8_t)(1  << 0)) /* ____0001 */
#define SINT16     ((uint8_t)(2  << 0)) /* ____0010 */
#define UINT16     ((uint8_t)(3  << 0)) /* ____0011 */
#define SINT32     ((uint8_t)(4  << 0)) /* ____0100 */
#define UINT32     ((uint8_t)(5  << 0)) /* ____0101 */
#define SINT64     ((uint8_t)(6  << 0)) /* ____0110 */
#define UINT64     ((uint8_t)(7  << 0)) /* ____0101 */
#define DOUBLE     ((uint8_t)(8  << 0)) /* ____1000 */
#define STRING     ((uint8_t)(9  << 0)) /* ____1001 */
#define POINTR     ((uint8_t)(10 << 0)) /* ____1010 */
#define DICOBJ     ((uint8_t)(11 << 0)) /* ____1011 (only use for dict)*/
//------------------------------------------------------------------------------
typedef struct COption {
    const char*     opname;
    const char*     ophelp;
    CType           optype; // option data type, match the option. 
    int32_t         offset; // option offset in the config struct.
    CAuto           defval; // def value, inited by the def value.
    CAuto           curval; // cur value, refer  of the var value.
    double          minval; // minimum valid value for the option.
    double          maxval; // maximum valid value for the option.
    const char*     module; // module to which the option belongs.
} COption;
#define OPTION_END_PLACEHOLDER  {NULL, NULL, -1, 0, 0, 0, 0, 0, NULL}
typedef struct CTpDesc {
    const char*     name; // unit name.
    CType           type; // unit type.
    int32_t         size;
    const char*     ctrl;
    CAuto           data;
} CTpDesc;
static CTpDesc CTypes[] = {
    {"SINT08",      SINT08,     sizeof(int8_t)      ,"%d"       ,{.i08=0}},
    {"UINT08",      UINT08,     sizeof(uint8_t)     ,"%u"       ,{.u08=0}},
    {"SINT16",      SINT16,     sizeof(int16_t)     ,"%d"       ,{.i16=0}},
    {"UINT16",      UINT16,     sizeof(uint16_t)    ,"%u"       ,{.i16=0}},
    {"SINT32",      SINT32,     sizeof(int32_t)     ,"%d"       ,{.i32=0}},
    {"UINT32",      UINT32,     sizeof(uint32_t)    ,"%u"       ,{.u32=0}},
    {"SINT64",      SINT64,     sizeof(int64_t)     ,"%lld"     ,{.i64=0}},
    {"UINT64",      UINT64,     sizeof(uint64_t)    ,"%llu"     ,{.u64=0}},
    {"DOUBLE",      DOUBLE,     sizeof(double)      ,"%.16lg"   ,{.dbl=0}},
    {"STRING",      STRING,     sizeof(char*)       ,"%s"       ,{.str=0}},
    {"POINTR",      POINTR,     sizeof(void*)       ,"%p"       ,{.ptr=0}},
    {"DICOBJ",      DICOBJ,     sizeof(void*)       ,"%p"       ,{.ptr=0}},
};
CLASS(CBuf)  { void*  buf; size_t   len; };
CLASS(CVar)  { CAuto  uni; uint64_t tag; };
CLASS(Pair)  { CVar*  key; CVar*    val; };
CLASS(CAlc) {
    void *(*malloc)(void *contex, size_t size);
    void *(*calloc)(void *contex, size_t nums, size_t size);
    void *(*ralloc)(void *contex, void *ptr, size_t size);
    void  (*free)  (void *contex, void *ptr);
    void  (*memrst)(void *contex); // Keep for mempool.
    void  (*delete)(void *contex); // Keep for mempool.
    /* A context for Alloc, can be NULL. */
    void *contex;
};
typedef bool(*cb_ptr)(void *args, void *item);
CLASS(Dtor)  {
    cb_ptr func; void* args; Dtor *next;
};
static void* malloc_(void *contex, size_t size) {
    return malloc(size);
}
static void* calloc_(void *contex, size_t nums, size_t size) {
    return calloc(nums, size);
}
static void* ralloc_(void *contex, void *ptr, size_t size) {
    return realloc(ptr, size);
}
static void  freept_(void *contex, void *ptr) {
    return free(ptr);
}
static CAlc* new_calc(bool mark) {
    CAlc *calk = (CAlc*)calloc(1, sizeof(CAlc));
    if (mark) { // default use system api.
        calk->malloc = malloc_;
        calk->calloc = calloc_;
        calk->ralloc = ralloc_;
        calk->free   = freept_;
    }
    return calk;
}
static void  del_calc(CAlc* calk) {
    if (calk->contex && calk->delete) {
        calk->delete(calk->contex);
        calk->contex = NULL;
    }
    free((void*)calk);
}
static Dtor* new_dtor(cb_ptr func, void *args) {
    Dtor* dtor = (Dtor*)malloc(sizeof(Dtor));
    dtor->func =  func;
    dtor->args =  args;
    dtor->next =  NULL;
    return dtor;
}
static bool  add_dtor(Dtor* head, Dtor* dtor) {
    if (NULL == head) {
        err("dtor head must be init first!\n");
        return false;
    }
    for (Dtor* p_dtor = head; NULL != p_dtor;
        p_dtor = p_dtor->next) {
        if (NULL ==  p_dtor->next) {
            p_dtor->next = dtor;
            break;
        }
    }
    return true;
}
static void  del_dtor(Dtor* dtor) {
    for (Dtor* p_dtor = dtor; NULL != p_dtor;) {
        Dtor* p_temp = p_dtor;
        p_dtor = p_dtor->next;
        free((void*)p_temp);
    }
}
//---------------------------------------------------------------------------------
static __thread int32_t nb_caches = 32, cur_index = 0;
static pthread_key_t  thr_key;
static pthread_once_t thronce = PTHREAD_ONCE_INIT;
static void del_caches(void* args) {
    if (NULL != args) {
        CBuf *caches = (CBuf*)args;
        for (int32_t i=0; i<nb_caches; ++i)
            free((void*)caches[i].buf);
        dbg("<TID:%d>caches=(%p) is freed!\n", gettid(), caches);
        free(caches); 
    }
}
static void cleanup(void) {del_caches(pthread_getspecific(thr_key));}
static void make_key() { 
    (void)pthread_key_create(&thr_key, del_caches); 
    atexit(cleanup);
}
static inline char* get_caches(int32_t need_size) {
    Assertor(0 == pthread_once(&thronce, make_key));
    CBuf *caches = (CBuf*)pthread_getspecific(thr_key);
    if (caches == NULL) { 
        caches = (CBuf*)calloc(nb_caches, sizeof(CBuf));
        Assertor(NULL != caches);
        Assertor(0 == pthread_setspecific(thr_key, caches));
        dbg("<TID:%d>caches=(%p) is alloc!\n", gettid(), caches);
    }
    cur_index = (cur_index+1)%nb_caches;
    if (caches[cur_index].len < need_size||caches[cur_index].len >= 4*1024) {
        caches[cur_index].buf = realloc(caches[cur_index].buf, need_size);
        caches[cur_index].len = need_size;
        Assertor(NULL != caches[cur_index].buf);
    }
    memset(caches[cur_index].buf, 0, caches[cur_index].len);
    return caches[cur_index].buf;
}
static inline bool is_innerptr(const char* ptr, const char* buf, int32_t len) {
    return (ptr >= buf && ptr < (buf+len));
}
#define is_intraptr(x, y, z)     is_innerptr((char*)x, (char*)y, (int32_t)z)
static inline bool is_cacheptr(char* ptr) {
    CBuf *caches = (CBuf*)pthread_getspecific(thr_key);
    if (caches) {
        for (int32_t i=0; i<nb_caches; ++i) {
            if (((char*)ptr >= caches[i].buf) &&
                (char*)ptr < ((char*)caches[i].buf + caches[i].len))
                return true;
        }
    }
    return false;
}

// 获取变量信息.
static inline const char* get_subtyp_desc(CVar* cvar) {
    CType styp = (CType)(cvar->tag & CTYPE_MASK);
    return (styp <= DICOBJ)? CTypes[styp].name : "INVALID";
}
static inline CType  get_subtyp(CVar* cvar) {
    return (CType)(cvar->tag & CTYPE_MASK);
}
static inline CType  get_vartyp(CVar* cvar) {
    return (CType)(cvar->tag & CTAGS_MASK);
}
static inline void   set_vartyp(CVar* cvar, CType vtp) {
    uint8_t mask = CTAGS_MASK & vtp;
    cvar->tag |= mask;
}
static inline bool   is_cvararr(CVar* cvar) {
    return cvar->tag & ARRPTR;
}
// 获取数组容量.
static inline size_t get_arrnum(CVar* cvar) {
    if (!is_cvararr(cvar)) {
        dbg("input isn't an arr!\n");
        return 0;
    }
    size_t  size = (size_t)(cvar->tag >> CTAGS_BITS);
    CType   styp = get_subtyp(cvar);
    return (size/CTypes[styp].size); 
}
// 获取数组元素；
#define get_arrval(ptr_, typ_, inx_) ({                                         \
    int8_t   *p_sint08 = NULL;                                                  \
    int16_t  *p_sint16 = NULL;                                                  \
    int32_t  *p_sint32 = NULL;                                                  \
    int64_t  *p_sint64 = NULL;                                                  \
    uint8_t  *p_uint08 = NULL;                                                  \
    uint16_t *p_uint16 = NULL;                                                  \
    uint32_t *p_uint32 = NULL;                                                  \
    uint64_t *p_uint64 = NULL;                                                  \
    double   *p_double = NULL;                                                  \
    char    **p_string = NULL;                                                  \
    void    **p_object = NULL;                                                  \
    switch (typ_) {                                                             \
        case SINT08: p_sint08 = (int8_t*)  ptr_; break;                         \
        case SINT16: p_sint16 = (int16_t*) ptr_; break;                         \
        case SINT32: p_sint32 = (int32_t*) ptr_; break;                         \
        case SINT64: p_sint64 = (int64_t*) ptr_; break;                         \
        case UINT08: p_uint08 = (uint8_t*) ptr_; break;                         \
        case UINT16: p_uint16 = (uint16_t*)ptr_; break;                         \
        case UINT32: p_uint32 = (uint32_t*)ptr_; break;                         \
        case UINT64: p_uint64 = (uint64_t*)ptr_; break;                         \
        case DOUBLE: p_double = (double*)  ptr_; break;                         \
        case STRING: p_string = (char**)   ptr_; break;                         \
        case POINTR: p_object = (void**)   ptr_; break;                         \
        case DICOBJ: p_object = (void**)   ptr_; break;                         \
        default: err("invalid type=%d!", typ_);  break;                         \
    }                                                                           \
    p_sint08?(CAuto)p_sint08[inx_]:                                             \
    p_sint16?(CAuto)p_sint16[inx_]:                                             \
    p_sint32?(CAuto)p_sint32[inx_]:                                             \
    p_sint64?(CAuto)p_sint64[inx_]:                                             \
    p_uint08?(CAuto)p_uint08[inx_]:                                             \
    p_uint16?(CAuto)p_uint16[inx_]:                                             \
    p_uint32?(CAuto)p_uint32[inx_]:                                             \
    p_uint64?(CAuto)p_uint64[inx_]:                                             \
    p_double?(CAuto)p_double[inx_]:                                             \
    p_string?(CAuto)p_string[inx_]:                                             \
    p_object?(CAuto)p_object[inx_]:(CAuto)0; })
// 获取变量大小(bytes)
// 注意：并非CVar自身的大小，而是cvar->uni指向的有效数据的大小；
// 对于字符串数组，获取的是[字符串指针-数组]大小 + 子串大小；
static inline size_t get_varlen(CVar* cvar) {
    size_t size = (size_t)(cvar->tag >> CTAGS_BITS);
    CType  styp = get_subtyp(cvar);
    if (!is_cvararr(cvar) || styp != STRING) {
        return size;
    } // only for string array.
    char**  str_arr = (char**)cvar->uni.ptr;
    for (int32_t i=0; i < get_arrnum(cvar); i++)
        size += strlen(str_arr[i]) + sizeof(char);
    return size;
}
// 对于字符串数组，设置的是[字符串指针-数组]大小.
static inline void   set_varlen(CVar *cvar, size_t len) {
    uint64_t tag = cvar->tag & CTAGS_MASK;
    tag |= (uint64_t)len << CTAGS_BITS;
    cvar->tag = tag;
}
// 构建CVar
static CVar* mkcvar_(CType vtyp, CAuto cval, int32_t nums) {
    CType   styp = CTYPE_MASK & vtyp; // get sub v type.
    int32_t size = 0, *size_arr = NULL;
    bool  is_arr = (ARRPTR & vtyp); 
    CVar* cvar = NULL;
    if (!is_arr) {
        size = (styp!=STRING) ? CTypes[styp].size : strlen(cval.str)+sizeof(char);
        cvar = (CVar*)calloc(1, sizeof(CVar) + size);
        dbg("is_arr=%d, cvar=%p, size=%d\n", is_arr, cvar, size);
        Assertor (NULL != cvar);
        if (styp == STRING) {
            cvar->uni.str = (char*)cvar + sizeof(CVar);
            memcpy(cvar->uni.str, cval.str, size);
        } else {
            cvar->uni = cval;
        }
    } else { // Array need alloc more memory.
        if (styp == STRING) { // 1.获取字串数组指针;
            size = CTypes[styp].size * nums; // avoid compute size twice.
            char** src_arr = (char**)cval.ptr; 
            size_arr = (int32_t*)calloc(nums, sizeof(int32_t));
            for (int32_t i=0; i < nums; i++) {
                size_arr[i]= strlen(src_arr[i]) + sizeof(char);
                size += size_arr[i];
                msg("src_arr[%d]=%s, size=%d\n",i, src_arr[i], size_arr[i]);
            }
            cvar = (CVar*)calloc(1, sizeof(CVar) + size);
            Assertor (NULL != cvar);
            char** dst_arr = (char**)((char*)cvar + sizeof(CVar));
            char*  pos_ptr = (char*)cvar + sizeof(CVar) + CTypes[styp].size * nums;
            for (int32_t i=0; i < nums; i++) {
                dst_arr[i] = pos_ptr;
                memcpy(dst_arr[i], src_arr[i], size_arr[i]);
                pos_ptr += size_arr[i];
            }
            cvar->uni.ptr = (void*)dst_arr;
            free((void*)size_arr);
            size = sizeof(char*) * nums;      // 存储的是nums个指针的数组；
        } else {            // 2.获取变量数组指针;
            dbg("is_arr=%d, styp=%d\n",is_arr, styp);
            void* src_arr = cval.ptr; 
            size += CTypes[styp].size * nums; // 存储的是nums个变量的数组；
            cvar = (CVar*)calloc(1, sizeof(CVar) + size);
            Assertor (NULL != cvar);
            void* dst_arr = (void*)((char*)cvar + sizeof(CVar));
            memcpy(dst_arr, src_arr, size);
            cvar->uni.ptr = (void*)dst_arr;
        }
    }
    set_vartyp(cvar, vtyp);
    set_varlen(cvar, size);
    return cvar;
}
// 拷贝CVar
static CVar* cpcvar(CVar* vdst, CVar* vsrc) {
    CType   styp = get_subtyp(vsrc);
    int32_t nums = get_arrnum(vsrc), size = 0, *size_arr = NULL;
    bool  is_arr = is_cvararr(vsrc); 
    if (!is_arr) {
        size = get_varlen(vsrc);
        vdst = vdst ? vdst : (CVar*)calloc(1, sizeof(CVar)+size);
        Assertor (NULL != vsrc);
        if (styp == STRING) {
            vdst->uni.str = (char*)vdst + sizeof(CVar);
            memcpy(vdst->uni.str, vsrc->uni.str, size);
        } else {
            vdst->uni = vsrc->uni;  //
        }
        vdst->tag = vsrc->tag;      //type and size.
    } else { // Array need alloc more memory.
        if (styp == STRING) { // 1.获取字串数组指针;
            size = CTypes[styp].size * nums; // avoid compute size twice.
            char** src_arr = (char**)vsrc->uni.ptr;
            size_arr = (int32_t*)calloc(nums, sizeof(int32_t));
            for (int32_t i=0; i < nums; i++) {
                size_arr[i]= strlen(src_arr[i]) + sizeof(char);
                size += size_arr[i];
                msg("src_arr[%d]=%s, size=%d\n",i, src_arr[i], size_arr[i]);
            }
            vdst = vdst ? vdst : (CVar*)calloc(1, sizeof(CVar) + size);
            Assertor (NULL != vdst);
            char** dst_arr = (char**)((char*)vdst + sizeof(CVar));
            char*  pos_ptr = (char*)vdst + sizeof(CVar) + sizeof(char*) * nums;
            for (int32_t i=0; i < nums; i++) {
                dst_arr[i] = pos_ptr;
                memcpy(dst_arr[i], src_arr[i], size_arr[i]);
                pos_ptr += size_arr[i];
            }
            vdst->uni.ptr = (void*)dst_arr;
            free((void*)size_arr);
            vdst->tag = vsrc->tag;  //type and size.
        } else {// 2.获取变量数组指针;
            size = get_varlen(vsrc);
            void* src_arr = vsrc->uni.ptr;
            vdst = vdst ? vdst : (CVar*)calloc(1, sizeof(CVar) + size);
            Assertor (NULL != vdst);
            void* dst_arr = (void*)((char*)vdst + sizeof(CVar));
            memcpy(dst_arr, src_arr, size);
            vdst->uni.ptr = (void*)dst_arr;
            vdst->tag = vsrc->tag; //type and size.
        }
    }
    return vdst;
}
#define PTR(x) ({void*    obj=(void*)   x; obj;})
#define OBJ(x) ({void*    obj=(void*)   x; obj;})
#define S64(x) ({int64_t  i64=(int64_t) x; i64;})
#define U64(x) ({uint64_t u64=(uint64_t)x; u64;})
#define mkcarr(tv, v, n)        mkcvar_(tv|ARRPTR, (CAuto)((void*)v), n)
#define mkcvar(tv, v)           mkcvar_(tv, (CAuto)v, 1)
#define mkcarr_nofree(tv, v, n) ({CVar* ptr_ = mkcarr(tv, v ,n);                \
    CVar  *dst_ = (CVar*)get_caches(get_varlen(ptr_) + sizeof(CVar));           \
    cpcvar(dst_, ptr_); free((void*)ptr_);dst_;})
#define mkcvar_nofree(tv, v) ({CVar* ptr_ = mkcvar(tv, v);                      \
    CVar  *dst_ = (CVar*)get_caches(get_varlen(ptr_) + sizeof(CVar));           \
    cpcvar(dst_, ptr_); free((void*)ptr_);dst_;})    
// 删除CVar.
static void  decvar(CVar** pptr) {
    if (NULL != pptr) {
        free((void*)(*pptr));
        *pptr = NULL;
    }
}
static char* strvar_alc(CVar* cvar, CAlc* alc) {
    CType   styp = get_subtyp(cvar);
    if (!is_cvararr(cvar)) { //
        int32_t size = 32;
        if (styp == STRING)
            size += strlen(cvar->uni.str) + sizeof(char);
        char *buff = (NULL != alc) ? 
            (char*)alc->calloc(alc->contex, 1, size) : (char*)calloc(1, size);
        if (styp == DOUBLE) {
            snprintf(buff, size, CTypes[styp].ctrl, cvar->uni.dbl);
        } else {
            snprintf(buff, size, CTypes[styp].ctrl, cvar->uni);
        }
        return buff;
    } else {
        int32_t nums = get_arrnum(cvar);
        int32_t size = 32 * nums; 
        if (styp == STRING)
            size += get_varlen(cvar);
        msg("@ is_arr=1! styp=%s, size=%d, nums=%d\n", 
            get_subtyp_desc(cvar), size, nums);
        char *buff = (NULL != alc) ? 
            (char*)alc->calloc(alc->contex, 1, size) : (char*)calloc(1, size);
        if (styp == DOUBLE) {
            for (int32_t i=0,offset=0; i < nums; i++) {
                snprintf(buff+offset, size, CTypes[styp].ctrl,
                    get_arrval(cvar->uni.ptr, styp, i).dbl);
                if (i < nums - 1) strcat(buff, ",");
                offset = strlen(buff);
            }
        } else {
            for (int32_t i=0,offset=0; i < nums; i++) {
                snprintf(buff+offset, size, CTypes[styp].ctrl,
                    get_arrval(cvar->uni.ptr, styp, i));
                if (i < nums - 1) strcat(buff, ",");
                offset = strlen(buff);
            }
        }
        return buff;
    }
}
#define strvar(cvar) strvar_alc(cvar, NULL)
#define strvar_nofree(cvar) ({char* ptr_ = strvar(cvar);                        \
    char  *dst_ = get_caches(strlen(ptr_) + sizeof(char));                      \
    strcpy(dst_, ptr_); free((void*)ptr_);dst_;})
static void debug_cvar(CVar *cvar){
    Assertor(NULL != cvar);
    CType styp = get_subtyp(cvar);
    if (is_cvararr(cvar)) {
        int size = get_varlen(cvar); // 如果是字符串数组，获取到的是[字符串指针-数组]大小；
        int nums = get_arrnum(cvar); // 单类型大小；
        msg("@ is_arr=1! styp=%s, size=%d, nums=%d, arr='%s'\n", 
            get_subtyp_desc(cvar), size, nums, strvar_nofree(cvar));
    } else {
        msg("@ is_arr=0! styp=%s, size=%d, nums=%d, var='%s'\n", 
            get_subtyp_desc(cvar), get_varlen(cvar), 1, strvar_nofree(cvar));
    }
}
// 构建Pair.
static Pair* mkpair_(CType ktyp, CAuto ckey, 
                CType vtyp, CAuto cval, int32_t nums) {
    bool is_arr = (ARRPTR & vtyp);
    Pair* pair = (Pair*)calloc(1, sizeof(Pair));
    Assertor(NULL != pair);
    pair->key = mkcvar(ktyp, ckey);
    pair->val = is_arr ? mkcarr(vtyp, cval.ptr, nums) : mkcvar(vtyp, cval);
    // debug_cvar(pair->val);
    return pair;
}
#define mkpair(tk, k, tv, v)            mkpair_(tk, (CAuto)k, tv, (CAuto)v, 1)
#define mkparr(tk, k, tv, v, n)                                                 \
        mkpair_(tk,(CAuto)k,tv|ARRPTR,(CAuto)((void*)v),n)
// 拷贝Pair.
static Pair* cppair(Pair* pair, Pair* psrc) {
    pair = pair ? pair : (Pair*)calloc(1, sizeof(Pair));
    Assertor(NULL != pair);
    pair->key = cpcvar(pair->key, psrc->key);
    pair->val = cpcvar(pair->val, psrc->val);
    return pair;
}
// 删除Pair.
static void  depair(Pair** pptr) {
    if (NULL != pptr) {
        Pair* pair = *pptr;
        if (NULL != pair) {
            decvar(&pair->key);
            decvar(&pair->val);
        }        
        free((void*)pair);
        *pptr = pair = NULL;
    }
}
#ifndef offsetof
//#define offsetof(type, member) ((size_t) &((type *)0)->member)
#define offsetof(type, member) ((size_t)((char*)(&((type*)1)->member) - 1))
#endif
#ifndef container_of
/**container_of - cast a member of a struct out to the containing structure
 * @ptr: the pointer to the member.
 * @type:the type of the container struct this is embedded in.
 * @mem: the name of the member within the struct.
 */
#define container_of(ptr, type, mem) (type*)((char*)ptr-offsetof(type,mem))
#endif

// HASH字典类型封装.
CLASS(IHtable) {
	int64_t 		key;
	void*			val;
	UT_hash_handle 	hh;
};

static void 
htable_insert(IHtable **hashtable, int64_t key, void *val) {
    IHtable *s;
    HASH_FIND_INT64(*hashtable, &key, s);  /* id already in the hash? */
    if (s == NULL) {
      s = (IHtable *) malloc(sizeof(IHtable));
      s->key = key;
      HASH_ADD_INT64(*hashtable, key, s);
    }
    s->val = val;  /* set value */
}

static void 
htable_update(IHtable **hashtable, int64_t key, void *val) {
    IHtable *s;
    HASH_FIND_INT64(*hashtable, &key, s);
    if (s!=NULL) {
        HASH_DEL(*hashtable, s);
        free(s);
    }

    s = (IHtable *) malloc(sizeof(IHtable));
    s->key = key;
    s->val = val;
    HASH_ADD_INT64(*hashtable, key, s);
}

static void* 
htable_search(IHtable **hashtable, int64_t key) {
    IHtable *s;
    HASH_FIND_INT64(*hashtable, &key, s);
    return s ? s->val : NULL;
}

// 寻找最接近的 SEI 数据（精确/近似均返回 val；勿在 s==NULL 时解引用）
static void*
htable_search_near(IHtable **hashtable, int64_t key) {
    IHtable *s = NULL, *p = NULL, *best = NULL;
    int64_t min_diff = INT64_MAX;

    if (!hashtable || !*hashtable)
        return NULL;

    HASH_FIND_INT64(*hashtable, &key, s);
    if (s != NULL) {
        dbg("[HITS] pts=%" PRId64 ", find an entry in hashtable!\n", key);
        return s->val;
    }

    for (p = *hashtable; p != NULL; p = p->hh.next) {
        int64_t diff = llabs(p->key - key);
        if (diff < min_diff) {
            min_diff = diff;
            best = p;
        }
    }
    if (!best)
        return NULL;

    war("[FIND] pts=%" PRId64 ", closest key=%" PRId64 ", diff=%" PRId64 "\n",
        key, best->key, min_diff);
    return best->val;
}

static void 
htable_delete(IHtable **hashtable, int64_t key, void (*clean)(void*)) {
    IHtable *s;
    HASH_FIND_INT64(*hashtable, &key, s);
    if (s != NULL) {
		if (NULL != clean) {
			dbg("clean key=%llu, %p\n", s->key, s->val);
			clean(s->val);
		}
      HASH_DEL(*hashtable, s);
      free(s);
    }
}

static unsigned int 
htable_counts(IHtable **hashtable) {
    return HASH_COUNT(*hashtable);
}

static void 
htable_prints(IHtable **hashtable) {
    IHtable *s;
    for(s=*hashtable; s != NULL; s=s->hh.next) {
        printf("key: %" PRId64 "\n", s->key);
        printf("val: %p\n", s->val); // 打印指针的值
    }
}

static void create_htable(IHtable **hashtable){
    *hashtable = NULL;
}

static void delete_htable(IHtable **hashtable, void (*clean)(void*)){
    IHtable *current_entry, *tmp;
    HASH_ITER(hh, *hashtable, current_entry, tmp) {
		// 回调清理资源.
		if (NULL != clean) {
    war("clean key=%" PRId64 "\n", current_entry->key);
			clean(current_entry->val);
		}
        HASH_DEL(*hashtable, current_entry);
        free(current_entry);
    }
}
#endif//__USE_CTYPES__

/*******************************************************************************/
/**Note: Multiple process or threads options.*/
// 线程|进程间互斥同步机制.
// 建议：多进程优先用unix域套接字同步(socketpair)-不丢消息，可移植性强;
// 此外，fork()之前最好不要启动多线程, 会有概率性的资源共享问题哈。
#ifdef __USE_THREAD__
#ifdef WIN32
// 原子操作, 针对内置类型.
#define atomic_set(x, y, arg...) 
#define atomic_and(x, y, arg...) 
#define atomic_orr(x, y, arg...) 
#define atomic_xor(x, y, arg...) 
#define atomic_non(x, y, arg...) 
#define atomic_add(x, y, arg...) 
#define atomic_sub(x, y, arg...) 
#define atomic_inc(x) 
#define atomic_dec(x) 
// 内存屏障, 无锁CAS 操作.                
#define membarrier(x, arg...) 
#define atomic_cas(x, y, z, arg...) 
#define ftime                       _ftime
#define timeb                       _timeb
#define mkdir_(x)                   mkdir(x)
#define msleep(x)                   Sleep(x)
#define __thread                    __declspec(thread)
#else// ref:[s-ns]=[time,ftime|gettimeofday,clock_gettime] 
#include <sys/sysinfo.h>
// 原子操作, 针对内置类型. return old.
#define atomic_set(x, y, arg...)    __sync_lock_test_and_set(&(x), y, ##arg)
#define atomic_and(x, y, arg...)    __sync_and_and_fetch(&(x), y, ##arg)
#define atomic_orr(x, y, arg...)    __sync_or_and_fetch (&(x), y, ##arg)
#define atomic_xor(x, y, arg...)    __sync_xor_and_fetch(&(x), y, ##arg)
#define atomic_non(x, y, arg...)    __sync_nand_and_fetch(&(x),y, ##arg)
#define atomic_add(x, y, arg...)    __sync_add_and_fetch(&(x), y, ##arg)
#define atomic_sub(x, y, arg...)    __sync_sub_and_fetch(&(x), y, ##arg)
#define atomic_inc(x)                 atomic_add(x, 1)
#define atomic_dec(x)                 atomic_sub(x, 1)
// 内存屏障, 无锁CAS 操作.    
#define membarrier(arg...)          __sync_synchronize(##arg)
#define atomic_cas(x, y, z, arg...) __sync_bool_compare_and_swap(&(x),y,z, ##arg)
#define mkdir_(x)                   mkdir(x, 0775)
#define msleep(x)                   usleep(x*1000)
#define localtime_s(x,y)            localtime_r(y,x)
static void* shmm_alloc(size_t size, const char* path) {
    void *addr = NULL;
    if (NULL != path) { //>> 没有亲缘关系的进程，必需指定path通信；
        int32_t fd = open(path, O_RDWR|O_CREAT|O_TRUNC, 0666);
        ftruncate(fd, size);
        addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    } else { // MAP_ANON 相当于fd = open("/dev/zero", O_RDWR);
        //addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED,-1, 0);
        int32_t fd = open("/dev/zero", O_RDWR);
        addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    }
    if (addr == MAP_FAILED) {
        err("Hi! mmap failed! %s\n", strerror(errno));
        return NULL;
    }
    return addr;
}
static void shmm_freep(void *addr, size_t size) {
    if (munmap(addr, size) == -1) {
        err("Hi! munmap(%p, %d) failed!", addr, (int)size);
    }
}
#endif

typedef void* (*thrd_func)(void*);
CLASS(IThread){
	char*          		name;	
	int32_t          	flag;		//0-invalid,  1-init.
	int32_t          	stat;		//0-joined,   1-detached.
	pthread_attr_t		attr;
	pthread_t 		 	hdwl;
	thrd_func 	  		func;
	void* 				args;
};

static bool
detect_thread(IThread* self) 
{
	return (self && (1 == self->flag) 
		 && (!pthread_kill(self->hdwl,0)));
}

static int32_t
detach_thread(IThread* self) 
{
 	Assertor(NULL != self);
	int32_t ret = -1;
	if (1 == self->flag)
		ret = pthread_detach(self->hdwl);
	return  ret;
}

static void
delete_thread(IThread** pptr) 
{
	if (pptr && *pptr) {
		IThread* self = *pptr;
		if (0 == self->stat)
			pthread_join(self->hdwl, NULL);
		if (NULL != self->name)
			free(self->name);
		free(self);
		*pptr = self = NULL;
	}
}

static IThread*
create_thread(char *name, thrd_func func, void *args) 
{
	Assertor(NULL != func);
	int32_t ret = 0;
	IThread* self = (IThread*)calloc(1, sizeof(IThread));		
	self->func = func;
	self->args = args;	
	self->name = (char*)calloc(1, strlen(name)+1);
	strcpy(self->name, name);
	ret = pthread_create(&self->hdwl, &self->attr, self->func, self->args);
	TryCatch(0 != ret, "pthread_create failed! %s", strerror(ret));
	self->flag = 1;	
	return self;
	
Exception:
	delete_thread(&self);
	return NULL;
}

CLASS(ISyncer){CSync type; ISyncer *lock; int32_t size; char data[0];};
static inline ISyncer* 
create_syncer(int tp) {
    int32_t ret = (SY_NO == tp) ? 0: -1, max_size = 0;     
    Assertor(tp >= SY_NO && tp < SY_UP);    
    max_size = max3(sizeof(pthread_mutex_t),                                   
        sizeof(pthread_spinlock_t), sizeof(pthread_rwlock_t));                 
    max_size = max3(max_size, sizeof(pthread_cond_t), sizeof(sem_t));
    ISyncer *p_  = NULL;
    switch (tp) {
        case SY_MU_SHARED:
        case SY_MR_SHARED:
        case SY_SP_SHARED:
        case SY_RW_SHARED:
        case SY_SM_SHARED:
        case SY_CO_SHARED:
            p_ = (ISyncer*)shmm_alloc(
                sizeof(ISyncer) + max_size, get_struuid("/tmp/shared_"));
            break;
        default:
            p_ = (ISyncer*)calloc(1, sizeof(ISyncer) + max_size);
            break;
    }    
    Assertor(NULL != p_); 
    p_->size = max_size;
    p_->type = tp; 
    dbg("sync_type=%02d, %s=%p!\n", tp, STR(p_), p_);
    if (SY_MU <= tp && SY_MR_SHARED >= tp) {
        if (SY_MU != tp) {
            pthread_mutexattr_t z;
            (void)pthread_mutexattr_init(&z);
            if (SY_MU_SHARED == tp || SY_MR_SHARED == tp) {
            (void)pthread_mutexattr_setpshared(&z, PTHREAD_PROCESS_SHARED);}
            if (SY_MR        == tp || SY_MR_SHARED == tp) {
            (void)pthread_mutexattr_settype(&z, PTHREAD_MUTEX_RECURSIVE);}
            ret = pthread_mutex_init((pthread_mutex_t*)p_->data, &z);
            (void)pthread_mutexattr_destroy(&z);
        } else {ret=pthread_mutex_init((pthread_mutex_t*)p_->data,NULL);}
    } else if (SY_SP == tp || SY_SP_SHARED == tp) {
        ret = pthread_spin_init((pthread_spinlock_t*)p_->data,(SY_SP!=tp));
    } else if (SY_RW == tp || SY_RW_SHARED == tp) {
        if (SY_RW_SHARED == tp){
            pthread_rwlockattr_t z;
            (void)pthread_rwlockattr_init(&z);
            (void)pthread_rwlockattr_setpshared(&z, PTHREAD_PROCESS_SHARED);   
            ret = pthread_rwlock_init((pthread_rwlock_t*)p_->data, &z);    
            (void)pthread_rwlockattr_destroy(&z);
        } else{ret=pthread_rwlock_init((pthread_rwlock_t*)p_->data,NULL);}
    } else if (SY_SM == tp || SY_SM_SHARED == tp) {
        ret = sem_init((sem_t*)p_->data,(SY_SM_SHARED==tp), 0);
    } else if (SY_CO == tp || SY_CO_SHARED == tp) {
            p_->lock = create_syncer((SY_CO== tp) ? SY_MU : SY_MU_SHARED);
            pthread_condattr_t z;
            (void)pthread_condattr_init(&z);
            (void)pthread_condattr_setclock(&z, CLOCK_MONOTONIC);
            if (SY_CO_SHARED == tp) {
            (void)pthread_condattr_setpshared(&z, PTHREAD_PROCESS_SHARED);}
            ret = pthread_cond_init((pthread_cond_t*)p_->data, &z); 
            (void)pthread_condattr_destroy(&z);
    }
    if (ret) { 
        err(": Error! (%s=%p, tp=%d) err='%s'!\n",STR(p_),p_, tp, strerror(ret));
        free((void*)p_); p_ = NULL;
    }
    return p_;
}

static inline void
delete_syncer(ISyncer *p_) {
    int32_t ret = 0;
    if(p_) {
        dbg("sync_type=%02d, %s=%p\n", p_->type, STR(p_), p_);
        ret = (SY_NO == p_->type) ? 0: -1;
        if (SY_MU <= p_->type && SY_MR_SHARED >= p_->type) 
            ret = pthread_mutex_destroy((pthread_mutex_t*)p_->data);
        if (SY_SP == p_->type || SY_SP_SHARED == p_->type)
            ret = pthread_spin_destroy((pthread_spinlock_t*)p_->data);
        if (SY_RW == p_->type || SY_RW_SHARED == p_->type)
            ret = pthread_rwlock_destroy((pthread_rwlock_t *)p_->data);
        if (SY_SM == p_->type || SY_SM_SHARED == p_->type)
            ret = sem_destroy((sem_t *)p_->data);
        if (SY_CO == p_->type || SY_CO_SHARED == p_->type) {
            delete_syncer(p_->lock);
            ret = pthread_cond_destroy((pthread_cond_t *)p_->data);}
        if (0 != ret)
            err("Error! (%s=%p, tp=%d) msg='%s'!\n",
                STR(p_), p_, p_->type, strerror(ret));
        switch (p_->type) {
            case SY_MU_SHARED:
            case SY_MR_SHARED:
            case SY_SP_SHARED:
            case SY_RW_SHARED:
            case SY_SM_SHARED:
            case SY_CO_SHARED:
                shmm_freep((void*)p_, sizeof(ISyncer) + p_->size); p_ = NULL;
            break;
        default:
            free((void*)p_); p_ = NULL;
            break;
        }
        Assertor(0 == ret);
    }
    return;
}
// 互斥加锁系列.
static inline int32_t 
syncer_dolock(ISyncer *x, int32_t _wr) { 
    int32_t ret = -1; 
    ISyncer *p_ = (ISyncer*)(x);
    Assertor(p_ != NULL);
    Assertor(p_->type >= SY_NO &&  p_->type <= SY_RW_SHARED);
    ret = (SY_NO == p_->type) ? 0: -1;
    if (SY_MU <= p_->type && SY_MR_SHARED >= p_->type)
        ret = pthread_mutex_lock((pthread_mutex_t*)p_->data); 
    if (SY_SP == p_->type || SY_SP_SHARED == p_->type) 
        ret = pthread_spin_lock((pthread_spinlock_t*)p_->data);
    if (SY_RW == p_->type || SY_RW_SHARED == p_->type) {
        if(_wr){ret = pthread_rwlock_wrlock((pthread_rwlock_t*)p_->data);  
        } else {ret = pthread_rwlock_rdlock((pthread_rwlock_t*)p_->data);}
    }
    Assertor(0 == ret); 
    return ret;
}
#define syncer_enlock(x)       syncer_dolock(x, 0)
#define syncer_rdlock(x)       syncer_dolock(x, 0)
#define syncer_wrlock(x)       syncer_dolock(x, 1)
static inline int32_t 
syncer_trydolock(ISyncer *x,int32_t  _wr) {
    int32_t ret = -1; 
    ISyncer *p_ = (ISyncer*)(x);
    Assertor(p_ != NULL);
    Assertor(p_->type >= SY_NO &&  p_->type <= SY_RW_SHARED);
    ret = (SY_NO == p_->type) ? 0: -1;
    if (SY_MU <= p_->type && SY_MR_SHARED >= p_->type)
        ret = pthread_mutex_trylock((pthread_mutex_t*)p_->data);
    if (SY_SP == p_->type || SY_SP_SHARED == p_->type)
        ret = pthread_spin_trylock((pthread_spinlock_t*)p_->data); 
    if (SY_RW == p_->type || SY_RW_SHARED == p_->type) {
        if(_wr){ret=pthread_rwlock_trywrlock((pthread_rwlock_t*)p_->data); 
        } else{ret=pthread_rwlock_tryrdlock((pthread_rwlock_t*)p_->data);}
    }
    return ret;
}
#define SyncerTryEnLock(x)    syncer_trydolock(x, 0)
#define SyncerTryRdLock(x)    syncer_trydolock(x, 0)
#define SyncerTryWrLock(x)    syncer_trydolock(x, 1)
static inline int32_t 
syncer_unlock(ISyncer *x) { 
    int32_t ret = -1;
    ISyncer *p_ = (ISyncer*)(x); 
    Assertor(p_ != NULL); 
    Assertor(p_->type >= SY_NO &&  p_->type <= SY_RW_SHARED);
    ret = (SY_NO == p_->type) ? 0: -1;
    if (SY_MU <= p_->type && SY_MR_SHARED >= p_->type)
        ret = pthread_mutex_unlock((pthread_mutex_t*)p_->data);  
    if (SY_SP == p_->type || SY_SP_SHARED == p_->type)
        ret = pthread_spin_unlock((pthread_spinlock_t*)p_->data);
    if (SY_RW == p_->type || SY_RW_SHARED == p_->type)
        ret = pthread_rwlock_unlock((pthread_rwlock_t*)p_->data);
    Assertor(0 == ret); return ret;
}
static inline int32_t 
syncer_smpost(ISyncer* p_) { 
    Assertor(p_ != NULL); 
    Assertor(p_->type == SY_SM || p_->type == SY_SM_SHARED);
    return sem_post((sem_t*)p_->data);
}
static inline int32_t 
syncer_trysmwait(ISyncer* p_) { 
    Assertor(p_ != NULL); 
    Assertor(p_->type == SY_SM || p_->type == SY_SM_SHARED);
    return sem_trywait((sem_t*)p_->data);
}
static inline int32_t 
syncer_smwait(ISyncer* p_, int32_t tm) {
    Assertor(p_ != NULL);
    Assertor(p_->type == SY_SM || p_->type == SY_SM_SHARED);
    
    int32_t ret = 0;
    dbg("%s=%p, timeout=%d!\n", STR(p_), p_, tm);
    if(tm <  0)
        ret = sem_wait((sem_t*)p_->data);
    else {
        struct timespec tv;
        clock_gettime(CLOCK_MONOTONIC, &tv);
        tv.tv_sec += tm/1000;
        uint64_t _ns = tv.tv_nsec + 1000000 * (tm % 1000);
        tv.tv_sec += _ns / 1000000000;
        tv.tv_nsec = _ns % 1000000000;
        ret = sem_timedwait((sem_t*)p_->data, &tv);
    }
    if (ret) err("err='%s'\n", strerror(errno));
    return ret;
}
// 同步信号系列.
#define syncer_notify(p_, b_, e_) ({ int32_t rc_ = -1;                          \
    Assertor(p_ != NULL);                                                       \
    Assertor(p_->type == SY_CO || p_->type == SY_CO_SHARED);                    \
    syncer_enlock(p_->lock); e_;                                                \
    if(b_ == 0) { rc_ = pthread_cond_signal((pthread_cond_t*)p_->data);         \
    } else { rc_ =   pthread_cond_broadcast((pthread_cond_t*)p_->data);}        \
    syncer_unlock(p_->lock); rc_;})
#define syncer_waitif(p_, tm, co) ({ int32_t rc_ =  0;                          \
    Assertor(p_ != NULL);                                                       \
    Assertor(p_->type == SY_CO || p_->type == SY_CO_SHARED);                    \
    syncer_enlock(p_->lock);                                                    \
    while(co) {                                                                 \
        if(tm <  0) { rc_ = pthread_cond_wait((pthread_cond_t*)p_->data,        \
                        (pthread_mutex_t*)p_->lock->data);                      \
        } else {                                                                \
            struct timespec tv;                                                 \
            clock_gettime(CLOCK_MONOTONIC, &tv);                                \
            tv.tv_sec += tm/1000;                                               \
            uint64_t _ns = tv.tv_nsec + 1000000 * (tm % 1000);                  \
            tv.tv_sec += _ns / 1000000000;                                      \
            tv.tv_nsec = _ns % 1000000000;                                      \
            rc_ = pthread_cond_timedwait(                                       \
                (pthread_cond_t*)p_->data, p_->lock->data, &tv);                \
        }                                                                       \
        if (rc_  != 0) {                                                        \
            if (rc_ != ETIMEDOUT) {                                             \
                err("SyncWaitIf:  err=%s\n", strerror(rc_)); abort();           \
            } break;                                                            \
        }                                                                       \
    }                                                                           \
    syncer_unlock(p_->lock); rc_; })
#endif//__USE_THREAD__
/*******************************************************************************/
#ifdef __USE_STRING__
/**Note: Extend file dirs or string  options.*/
#define strfreep(s_) ({free((void*)s_); s_=NULL;})
static inline int32_t
makedirs(const char *path) {
    if (NULL == path) {
        err("invalid dir=%p\n", path);
        return -1;
    }
    if (0 == access(path, F_OK))
        return 0;
    int32_t size = (int32_t)strlen(path);
    char   *buff = (char*)malloc(size + sizeof(char)), *pos = NULL;
    strcpy(buff, path);
    if (buff[size - 1] == '/')
        buff[size - 1] = '\0';
    if (mkdir_(buff) == -1) {
        pos = buff + 1;
        while (1) {     //mkdir by recursive!
            while (*pos && *pos != '\\' && *pos != '/')
                pos++;
            char hold = *pos;
            *pos = 0;
            if (mkdir_(buff) == -1 && errno != EEXIST){
                err("%s:%s\n", buff, strerror(errno));
                free(buff);
                return -1;
            }
            if (hold == 0)
                break;
            *pos++ = hold;
        }
    }
    free(buff);
    return 0;//mkdir once success!
}
static inline int32_t
makefile(const char * path) {
    if (0 == access(path, F_OK)) {
        msg("'Hi! %s' is already exist!\n", path);
        return 0;
    } else {
		char* pdir = strdup(path);
		if (-1 == makedirs(dirname(pdir))) {
			strfreep(pdir);
			return -1;
		}
		strfreep(pdir);
	}
    return fclose(fopen(path, "wb+"));
}
static inline  bool
dumpfile(const char* path, const char* buff, int32_t size, const char* mode) {
    int32_t result = 0;
    FILE* fp = fopen(path, mode?mode:"wb+");
    if (NULL == fp) {
        err("open '%s' failed! %s", path, strerror(errno));
        return NULL;
    }
    result = fwrite(buff, sizeof(char), size, fp);
    if (result != size) {
        err("fwrite:%s!\n", strerror(errno));
        return false;
    }
    return true;
}
static inline  char*
readfile(const char* path) {
    if (0 != access(path, F_OK))
        return NULL;
    int32_t size  = 0, result = 0;
    void* buff = NULL;
    FILE* fp = fopen(path, "rb");
    if (NULL == fp) {
        err("open '%s' failed! %s", path, strerror(errno));
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    buff = calloc(1, size);
    Assertor(NULL != buff);
    rewind(fp);
    result = fread(buff, sizeof(char), size, fp);
    if (result != size) {
        err("fread:%s!\n", strerror(errno));
        free(buff);
        return NULL;
    }
    (void)fclose(fp);
    return (char*)buff;

}
static inline  char*
readline(const char* path) {
    int32_t size  = 0, result = 0, loops = 0;
    void *buff = NULL;
    FILE* fp = fopen(path, "rb");
    if (NULL == fp) {
        err("open '%s' failed! %s", path, strerror(errno));
        return NULL;
    }
    #define   once_size   1024
    char temp[once_size] = {0};
    while (!feof(fp)) {
        result = fread(temp, sizeof(char), once_size, fp);
        for (int32_t i=0; i< result; ++i) {
            if (temp[i] != '\n')
                continue;
            size = loops*once_size + i; //记录偏移值.
            break; 
        }
        if (0 != size) {
            break;
        }
        loops++;
    }
    if (0 == size) { // read whole file.
        return readfile(path);
    }
    buff = calloc(1, size+sizeof(char));
    Assertor(NULL != buff);
    (void)fread(buff, sizeof(char), size, fp);
    (void)fclose(fp);
    return (char*)buff;
}
static inline char* 
stralloc(int32_t size, const char *str) {
    int32_t dst_size = size+(str?strlen(str):0);
    char* dst = (char*)calloc(1, dst_size + 1);
    Assertor(NULL != dst); //aovid memory leak.
    return (str?strcpy(dst, str):dst);
}
static inline char* 
strclone(const char *src) {
    return src ? strdup(src) : NULL;
}
static inline char** 
strarrcp(const char **arr, int32_t nums) {
    int32_t *arr_size = (int32_t*)calloc(1, nums*sizeof(int32_t));
    int32_t  dst_size = (nums+1)*sizeof(char*), src_size = 0, i=0;
    for (i=0; i<nums; ++i) {
        src_size += (arr_size[i] = strlen(arr[i])+sizeof(char));
    }
    char *src = NULL, *p_src, **dst = NULL;
    dst = (char**)calloc(1, dst_size + src_size);
    src = p_src = (char*)(dst + nums+1);
    for (i=0; i<nums; ++i) {
        dst[i] = p_src;
        strcpy(p_src, arr[i]); p_src += arr_size[i];
    }
    dst[nums] = NULL; // mark char* arr[] end.
    free((void*)arr_size);
    dbg("i=%d, nums=%d\n", i, nums);
    return dst;
}
// success return >= 0, never failed.
static inline int32_t
strcount(const char *src, const char *sub) {
    int32_t count = 0;
    char *ptr = NULL;
    while ( ptr = strstr(src, sub)) {
        src += ((ptr - src)+strlen(sub));
        count++;
    }
    return count;
}
// success pStrNew, failed with NULL.
static inline char* 
strrepls(const char *str, const char *fr, const char *to) {
    int32_t nb_match = strcount(str, fr), offset = 0;
    int32_t src_size = strlen(str) + sizeof(char);
    int32_t fr_size  = strlen(fr), to_size = strlen(to);
    int32_t dst_size = src_size + nb_match*(to_size-fr_size);
    char *dst = calloc(1, dst_size), *ptr = NULL;
    const char *src = str;
    while ( (ptr = strstr(src, fr)) ) {
        snprintf(dst+offset, ptr-src+1, "%s",src);
        offset = strlen(dst);
        snprintf(dst+offset, to_size+1, "%s",to);
        offset = strlen(dst);
        src += ((ptr-src)+strlen(fr));
    }
    snprintf(dst+offset, dst_size-offset, "%s", src);
    return (!nb_match) ? strcpy(dst, str) : dst;
}
// reused data avoid freq alloc/free
// BUG: print(...) parms > BLOCKS=16(output by xxx_nofree api), maybe crash!
#define strrepls_nofree(sr_, fr_, to_) ({char* ptr_ = strrepls(sr_, fr_, to_);  \
    char  *dst_ = get_caches(strlen(ptr_) + sizeof(char));                      \
    strcpy(dst_, ptr_); free((void*)ptr_);dst_;})                               \
// success return > 0, <=0 is failed.
static inline int32_t 
nb_split(const char* input, const char *delim) {
    Assertor(NULL != input && NULL != delim);
    int32_t dst_num = 0, src_size = strlen(input)+sizeof(char);
    char *src = calloc(1, src_size), *ptr = NULL;
    strcpy(src, input);
    // compute the str nums.
    ptr = strtok(src, delim);
    dst_num++;
    while(ptr = strtok(NULL, delim))
        dst_num++;
    dbg(">>> src_size=%d, split=%d by delim='%s'!\n", src_size, dst_num, delim);
    free((void*)src);
    return dst_num;    
}
// success pStrArr, failed with NULL.
static inline char**
strsplit(const char* str, const char *dot, int32_t *num) {
    Assertor(NULL != str && NULL != dot);
    int32_t dst_num = nb_split(str, dot);
    if (num) (*num)= dst_num;
    int32_t dst_size = (dst_num+1)*sizeof(char*); //存指针.
    int32_t src_size = strlen(str)+sizeof(char), i = 0;
    char **dst = (char**)calloc(1, dst_size + src_size), *src = NULL;
    src = (char*)(dst + dst_num + 1);
    strcpy(src, str);
    // split and store string.
    char *ptr = strtok(src, dot);
    dst[i++] = ptr ? ptr : src;
    while(ptr = strtok(NULL, dot))
        dst[i++] = ptr;
    dst[i] = NULL;
    dbg("i=%d, dst_num=%d\n", i, dst_num);
    return dst;    
}
static inline char* 
strmerge(const char *str, ...) {
    const char *arg = str;
    va_list ap; va_start(ap, str);  //获得不定参数的首地址
    int32_t dst_size = 128, tmp_size = 0;
    char*   dst = (char*)calloc(1, dst_size);
    while(arg) {
        tmp_size += strlen(arg);//
        dbg("tmp_size=%d,dst_size=%d, %s\n", tmp_size, dst_size, arg);
        if (tmp_size >= dst_size) {
            msg("realloc\n");
            dst_size *= 2;
            dst = (char*)realloc(dst, dst_size);
            Assertor(NULL != dst); //aovid memory leak.
        }
        strcat(dst, arg);
        arg = va_arg(ap, const char*);
    }
    return dst;
}
// reused data avoid freq alloc/free
// BUG: print(...) parms > BLOCKS=16(output by xxx_nofree api), maybe crash!
#define strmerge_nofree(str, arg ...) ({char* ptr_ = strmerge(str, ## arg);     \
    char  *dst_ = get_caches(strlen(ptr_) + sizeof(char));                      \
    strcpy(dst_, ptr_); free((void*)ptr_);dst_;})                               \
// success pStrArr,will never failed.
static inline char*
strstrip(char *s) {
    size_t size = 0;
    char *end = NULL;
    size = strlen(s);
    if (!size)
        return s;
    end = s + size - 1;
    while (end >= s && isspace(*end))
        end--;
    *(end + 1) = '\0';
    while (*s && isspace(*s))
        s++;
    return s;
}
//UTF-8字符长度1-6， 可以根据每个字符第一个字节判断整个字符长度
//0xxxxxxx
//110xxxxx 10xxxxxx
//1110xxxx 10xxxxxx 10xxxxxx
//11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
//111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
//1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
//定义查找表，长度256，表中数值表示以此为起始字节的utf8字符长度
static unsigned char utf8_mapper[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,

    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,

    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,

    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 1, 1
};
/**
 *@brief 计算字符串字符数目.
 * 注意:不是字节个数.
 */
static inline int32_t  
utf8_strlen(const char *str) {
    uint32_t len = 0;
    for(const char *ptr = str; *ptr!='\0' && len<strlen(str); 
        len++, ptr += utf8_mapper[(unsigned char)*ptr]);
    return len;
}
/**
 *@brief 获取指定字符串子串.
 *注意:返回的指针无需释放.
 */
static inline char* 
utf8_strstr(const char *src, const char *sub) {
    return strstr(src, sub);
}
/**
 *@brief 获取指定的区间子串.
 * 注意:返回的指针需要释放.
 */
static inline char* 
utf8_strcut(const char *str, int32_t beg, int32_t end) {
    int32_t len = utf8_strlen(str);
    if(beg >= len) return NULL;
    if(end >= len) end = len;
    
    const char *beg_ptr = str;
    for(int32_t i = 0  ; i < beg; ++i,
        beg_ptr += utf8_mapper[(unsigned char)*beg_ptr]);
    const char *end_ptr = beg_ptr;
    for(int32_t i = beg; i < end; ++i,
        end_ptr += utf8_mapper[(unsigned char)*end_ptr]);
    int32_t retLen = (int32_t)(end_ptr - beg_ptr); // 子串字节数.
    char *retStr = (char*)malloc(retLen+sizeof(char));
    memcpy(retStr, beg_ptr, retLen);
    retStr[retLen] = '\0';
    return retStr;
}
static inline char* 
utf8_strrep(const char *src, const char *fr, const char *to) {
    return strrepls(src, fr, to);
}
static inline char**
utf8_strspl(const char* src, const char *dot, int32_t *num) {
    return strsplit(src, dot, num);
}
static inline char* 
utf8_strcat(const char *src, const char *str) {
    return strmerge(src, str, NULL);
}
/**
 *@brief  转换enfr编码到ento编码. 
 * 使用： iconv -l查看支持的编码.
 *@param  ndst 必须足够放的下转换后的字符串.
 *           ndst>=4*nsrc, 足够存放.
 *@retval 返回pdst字符串长度>0,错误<=0.
 */
static inline int32_t 
utf_convert(char* enfr, char* ento, 
        char *psrc, int32_t nsrc, char *pdst, int32_t ndst) {
    assert(NULL != psrc && NULL != pdst);
    
    char*    src         = psrc;// 由于iconv()会修改指针,故要保存源指针
    size_t  src_size     = nsrc;
    char*    dst         = pdst;
    size_t  dst_size     = ndst;
    
    /** 获得转换句柄 
     *@param encTo 目标编码方式
     *            TRANSLIT：遇到无法转换的字符就找相近字符替换
     *            IGNORE  ：遇到无法转换字符跳过
     *@param encFr 源流编码方式             */    
    iconv_t conv_hdwl = iconv_open (ento, enfr);
    if (conv_hdwl == (iconv_t)-1) {
        perror ("iconv_open");
        return -1;
    }
    //printf ("111->>>src:%s, src_size:%d, dst: %s dst_size:%d\n",
    //      src, src_size, dst, dst_size);
    /** 进行字符转换
     *@param src             需要转换的字符串
     *@param src_size         存放还有多少字符没有转换
     *@param dst             存放转换后的字符串
     *@param dst_size         存放转换后,dst剩余的空间 */
    int32_t ret = iconv (conv_hdwl, &src, &src_size, &dst, &dst_size);
    if (ret == -1) {
        perror ("iconv");
        return -2;
    }
    /* 关闭转换句柄*/
    iconv_close (conv_hdwl);
    //printf ("222->>>src:%s, src_size:%d, dst: %s dst_size:%d\n", 
    //      src, src_size, dst, dst_size);
    return (ndst-dst_size);
}
#define utf82unic(s,x,d,y)     utf_convert("UTF-8","UNICODE//IGNORE",s,x,d,y)
#define unic2utf8(s,x,d,y)     utf_convert("UNICODE//IGNORE","UTF-8",s,x,d,y)
static inline bool
is_net_pro(const char* path) {
    return (strstr(path, "http:") || strstr(path, "https:")
        ||  strstr(path, "rtmp:") || strstr(path, "rtmpt:")
        ||  strstr(path, "rtsp:") || strstr(path, "rtmpe:") );
}
static inline bool
is_ZHstring(const char* str) {
    for (uint32_t i = 0; i < strlen(str); i++)
        if (*( str+i) & 0x80)
            return 1;
    return 0;
}
static inline bool
is_utf8_enc(void) {
    return (3 == strlen("啊") && 1==strlen("a"));
}
#endif //#define __USE_STRING__
/*******************************************************************************/
#ifdef __USE_TIMMER__
// common timer implement.
CLASS(ITimer) {
    int32_t state;
    timer_t timer;
    int32_t index;	
    int32_t exe_counts;
    int32_t max_counts;
	int64_t start_time;
	void  (*func)(void*,void*);
	void   *func_args;
};

static void
set_timer(union sigval sv) {
    ITimer *self = (ITimer *)sv.sival_ptr;
    self->func(self, self->func_args);
    self->exe_counts++;  
    if (self->max_counts > 0 && self->exe_counts >= self->max_counts) {
      	war("@Timer [%02d] reached the max loop counts=%d, stopped!\n", self->index, self->max_counts);
		if (timer_delete(self->timer) == -1) {
			perror("timer_delete");
		}
		self->state = 0;
    }
}

/**
 * brief: create timer instanse.
 * param: firstrun|interval ms.
 */
static ITimer* 
new_timer(int index, int firstrun, int interval, int max_counts, void (*func)(void*, void*), void *func_args) {
	int32_t ret = 0;
    struct sigevent sev;
    ITimer *self = (ITimer*)calloc(1, sizeof(ITimer));
    Assertor(self != NULL);
	
    self->index = index;
    self->exe_counts = 0;
    self->max_counts = max_counts;
    self->start_time = av_gettime_relative(); // us.
    self->func = func;
    self->func_args = func_args;
	
    // 指定处理函数.
    // - `SIGEV_NONE`：到期后无操作。
 	// - `SIGEV_SIGNAL`：发送指定信号 (`sigev_signo`)。
  	// - `SIGEV_THREAD`：调用指定的函数 (`sigev_notify_function`)。
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = self;
    sev.sigev_notify_function = set_timer;
    sev.sigev_notify_attributes = NULL;

    ret = timer_create(CLOCK_REALTIME, &sev, &self->timer);
	TryCatch(0 != ret, "timer_create failed! %s", strerror(errno));
	// 设定间隔时间.
    struct timespec firstrun_ts, interval_ts;
#define msec_to_timespec(msec, ts)		\
    do { 										\
		(ts)->tv_sec = (msec) / 1000; (ts)->tv_nsec = ((msec) % 1000) * 1000000; \
    } while (0)
    msec_to_timespec(firstrun, &firstrun_ts);
    msec_to_timespec(interval, &interval_ts);
    struct itimerspec its;
    its.it_value 	= firstrun_ts; 	// 初始时间
    its.it_interval = interval_ts; 	// 间隔时间
    ret = timer_settime(self->timer, 0, &its, NULL);
	TryCatch(0 != ret, "timer_settime failed! %s", strerror(errno));	
	self->state = 1;
	
	msg("Hi! create timer-[%d] success!\n", self->index);
    return self;
Exception:
	return NULL;
	
}

static void 
del_timer(ITimer **pptr) {
	if (pptr && *pptr) {
		msg("Hi! delete timer-[%d] success!\n", (*pptr)->index);
		ITimer* self = *pptr;		
		if (self->state && timer_delete(self->timer) == -1) {
			perror("timer_delete");
		}
		free((void*)self);
		*pptr = self = NULL;
	}
}

/**Note: system limits or systime(r) options.*/
/**
 * @brief  输出当前或者开机时间
 * @param  type-时间类型[0-当前, 1-开机时间]
 * @param  prec-精度类型[0|1|2|3=s|ms|us|ns]
 * @return <=0 出错, >0 时间值.
 */
static int64_t 
get_systime(bool type, int32_t prec) {
    assert( prec >=0 && prec <= 3 );
    uint64_t prec_ = 1000000000llu;
    for ( int32_t i = 0; i < prec; ++i )
        prec_  /= 1000;
#ifdef WIN32 //开机时间/当前时间(ms).
    int64_t ts = type?GetTickCount():get_curtime();
    return ts*1000000llu/prec_;
#else
    #ifdef __STRICT_ANSI__
        err("%s need compile 'gcc -std=gnu**'.\n", __FUNCTION__);
        return -1;
    #else
    struct timespec ts = {0, 0};
    clock_gettime((type?CLOCK_MONOTONIC_RAW:CLOCK_REALTIME), &ts);
    return (ts.tv_sec*1000000000llu+ts.tv_nsec)/prec_;
    #endif
#endif
}
/**
 *@brief 长整数输出当前时间(ms)
 */
static int64_t 
get_curtime(void) {
#if ( __STDC_VERSION__ >= 201112L ) //Note: support c11.
    struct timespec ts = {0, 0};
    timespec_get(&ts, TIME_UTC);    
    return ((ts.tv_sec*1000000000llu+ts.tv_nsec)/1000000);
#endif
    struct timeb tb = { 0 };
    (void)ftime(&tb);
    return ((int64_t)tb.time * 1000 + tb.millitm);
}

/**
 *@brief 格式化输出当前时间(ts)
 */
static inline char* 
get_fmttime(void) {
    static __thread char ts[128] = { 0 };
    struct timeb tb = { 0 };
    (void)ftime(&tb);
    struct tm now_time;
    struct tm *tm = localtime_s(&now_time, &tb.time);    
    snprintf(ts, sizeof(ts), "%4d-%02d-%02d %02d:%02d:%02d,%03d",
            tm->tm_year+1900,tm->tm_mon+1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec,tb.millitm);
    return ts;
}
static inline char*
get_struuid(const char *str) {
    static __thread char buff[128] = { 0 };
    static uint64_t count = 0;
    snprintf(buff, sizeof(buff),
        "%s%010llu+%020lld", str,
        (unsigned long long)atomic_inc(count),
        (long long)get_systime(1,3));
    return buff;
}
#endif //#define __USE_TIMMER__
/*******************************************************************************/
#ifdef __USE_SYSTEM__
/**Note: system limits or systime(r) options.*/
static inline int32_t 
get_cpunums(void) {
#if defined(WIN32)|| defined(WIN64)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
#elif defined(__linux__) || defined(__unix__)
    return get_nprocs();   //GNU fuction
#else
    return 0;
#endif
}
static inline void 
print_stack(void) {
#define max_ptr_nums 100
    void  *pbuff[max_ptr_nums] = { 0 };
    size_t nptrs  = backtrace(pbuff, max_ptr_nums);
    char **symbols = (char **)backtrace_symbols(pbuff, nptrs);
    for(size_t i = 0; i<nptrs; i++) {
        err("#%02zu: %s\n", i, symbols[i]);
    }
    free(symbols);
    err("use: addr2line -e {bin} -f {addr}, for details!\n");
    return;
}
#endif //#define __USE_SYSTEM__

/*******************************************************************************/
/**Note: network process or socket options.*/
// 网络|网络库封装机制.
// 建议：尽量使用zmq进行网络编程，可移植性强;
#ifdef __USE_SOCKET__
// 封装zeromq数据类型.
CLASS(IZeromq) {
	char* 		zurl; 	// zmq://.../...
	int32_t		type; 	// ZMQ_REQ/ZMQ_REP/ZMQ_PUB/ZMQ_SUB/ZMQ_PUSH/ZMQ_PULL...
	bool		bind; 	// [0-caller, 1-server], default: 0 
	void*		zctx;	// default bind 1 socket for rapid.
	void*		sock;
	// contex options.
	int32_t     nthr;	// 1.
	int32_t 	nsoc;	// 1024.
	// socket options.
	bool        opts; 	// default enable user socket options.
};

#include <zmq.h>
/**
	 *@brief 释放ZMQ实例.
 */
static void 
delete_zeromq(IZeromq** pptr)
{
	if (pptr && *pptr) {
		IZeromq* self = *pptr;
		msg("Hi! delete IZeromq (zurl:%s, zctx:%p, sock:%p, nthr=%d, nsoc=%d, opts=%d) success!\n",
			self->zurl, self->zctx, self->sock, self->nthr, self->nsoc, self->opts);
		if (self->sock) {
			zmq_close(self->sock);
			self->sock = NULL;
		}
		if (self->zctx) {
			zmq_ctx_destroy(self->zctx);
			self->zctx = NULL;
		}
		free((void*)self->zurl);
		free((void*)self);
		*pptr = self = NULL;
	}
}
/**
	 *@brief 配置ZMQ实例.
 */
static bool
config_zeromq(IZeromq* self)
{
    Assertor(self != NULL);
	int32_t ret = -1;
	// set contex optons.
	ret = zmq_ctx_set(self->zctx, ZMQ_IO_THREADS , self->nthr);
	TryCatch(0 != ret, "create zmq contex failed! %s", zmq_strerror(errno));
	ret = zmq_ctx_set(self->zctx, ZMQ_MAX_SOCKETS, self->nsoc);
	TryCatch(0 != ret, "create zmq contex failed! %s", zmq_strerror(errno));
	// set socket options.
	int32_t linger = 0;
	ret = zmq_setsockopt(self->sock, ZMQ_LINGER,     &linger,  sizeof(linger));	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	if (!self->opts) 
		return 1;
	int32_t one_pkt = 0; // 1-enable will disable ZMQ_SNDHWM && ZMQ_RCVHWM
	ret = zmq_setsockopt(self->sock, ZMQ_CONFLATE,  &one_pkt, sizeof(one_pkt)); 	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	int32_t max_hwm = 8;
	ret = zmq_setsockopt(self->sock, ZMQ_SNDHWM,	&max_hwm, sizeof(max_hwm)); 	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	ret = zmq_setsockopt(self->sock, ZMQ_RCVHWM,	&max_hwm, sizeof(max_hwm)); 	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	int32_t max_buf = 4*1024*1024; // 2*max_hwm*sizeof(UserData);
	ret = zmq_setsockopt(self->sock, ZMQ_SNDBUF,	&max_buf, sizeof(max_buf)); 	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	ret = zmq_setsockopt(self->sock, ZMQ_RCVBUF,	&max_buf, sizeof(max_buf)); 	
	TryCatch(0 != ret, "create zmq socket failed! %s", zmq_strerror(errno));
	msg("Hi! config IZeromq (zurl:%s, zctx:%p, sock:%p, nthr=%d, nsoc=%d, opts=%d) success!\n",
		self->zurl, self->zctx, self->sock, self->nthr, self->nsoc, self->opts);
	return 1;
Exception:
	return 0;
}

/**
	 *@brief 创建ZMQ实例.
	 *@param face-复用存在的实例.
	 *		eg. new if null.
	 *@param zurl-socket路径.
	 * 		eg. zmq://xxx/xx.
	 *@param type-socket模型.
	 * 		eg. ZMQ_REQ/ZMQ_REP/ZMQ_PUB/ZMQ_SUB......
	 *@param zurl-socket路径.
 */
static IZeromq* 
create_zeromq(int32_t type, bool bind, const char* zurl)
{
	char* path = strclone(zurl);
	char *p = strstr(path, "ipc://");
	if (p) { // substring found
		p += strlen("ipc://"); // Skip past the "ipc://" part
		Assertor(makedirs(dirname(p)) != -1);
	}
	strfreep(path);
	int32_t ret = -1;
	IZeromq* self = (IZeromq*)calloc(1, sizeof(IZeromq));		
	self->type = type;	
	self->zurl = (char*)calloc(1, strlen(zurl)+sizeof(char));
	strcpy(self->zurl, zurl);
	// contex.
	self->zctx = zmq_ctx_new();/// 创建一个新的环境
    Assertor(self->zctx != NULL);
	// socket.
	self->sock = zmq_socket(self->zctx, type);
    Assertor(self->sock != NULL);
	if (type == ZMQ_SUB) { // 必须对消息滤波，否则接受不到消息
		ret = zmq_setsockopt(self->sock, ZMQ_SUBSCRIBE, "", 0);
		TryCatch(0 != ret, "create zmq socket failed! %s", strerror(ret));
	}
	self->type = type;
	if (bind) {
		ret = zmq_bind(   self->sock, zurl);	// 绑定.
	} else {
		ret = zmq_connect(self->sock, zurl); 	// 连接.
	}
	TryCatch(0 != ret, "create zmq socket(%s) failed! %s", self->zurl, zmq_strerror(errno));
	self->bind = bind;
	self->nthr = 1; 
	self->nsoc = 1024;
	self->opts = 1; // 默认开启个性配置.
	(void)config_zeromq(self);
	msg("Hi! create IZeromq (zurl:%s, zctx:%p, sock:%p) success!\n", self->zurl, self->zctx, self->sock);
	return self;
Exception:
	delete_zeromq(&self);
	return NULL;
}

/**
 	*@brief 发送数据：check revents & ZMQ_POLLOUT. 
	*@param timeout =-1 block, >=0 单位: ms.
	*@rcode -1，错误存储在errno中. >=0 size.
*/
static int32_t
zmqbuf_send(IZeromq* self, void* buff, size_t size, long timeout) {
	Assertor (self != NULL && buff != NULL);
    zmq_pollitem_t items = { 
		.socket = self->sock, .fd = 0, .events = ZMQ_POLLOUT, 
		.revents = 0 }; 
	TryCatch(-1 == (zmq_poll(&items, 1, timeout)),
		"zmqbuf_send failed! %s", zmq_strerror(errno));
	if (!(items.revents & ZMQ_POLLOUT))
		goto Exception;
	TryCatch((zmq_send(self->sock, buff, size, ZMQ_DONTWAIT) == -1),
		"zmqbuf_send failed: %s", zmq_strerror(errno));
	dbg("@buff[%zu]='%s'\n", size, (char *)buff);
	return size;
Exception:
	return -1;
}
/**
	 *@brief 接收数据：check revents & ZMQ_POLLIN. 
	 *@param buff, 
	 * ==NULL, need freed by caller.
	 * !=NULL, caller alloc.
	 *@param capa, bytes caller expect.
	 *@param timeout =-1 block, >=0 单位: ms.
	 *@rcode -1，错误存储在errno中. >=0 size.
*/
static int32_t
zmqbuf_recv(IZeromq* self, void** buff, size_t capa, long timeout) {
	Assertor (self != NULL && buff != NULL);
    zmq_pollitem_t items = { 
		.socket = self->sock, .fd = 0, .events = ZMQ_POLLIN, 
		.revents = 0 }; 
	TryCatch(-1 == (zmq_poll(&items, 1, timeout)),
		"zmqbuf_recv failed! %s", zmq_strerror(errno));
	if (!(items.revents & ZMQ_POLLIN))
		goto Exception;
	if (*buff != NULL) {
		Assertor (capa > 0);
	} else {
		*buff = calloc(1, capa); // auto alloc .
	}
	Assertor (*buff != NULL);
    size_t size = 0;
    TryCatch((size = zmq_recv(self->sock, *buff, capa, ZMQ_DONTWAIT)) == -1,
		"zmqbuf_recv failed: %s", zmq_strerror(errno));
	dbg("@buff[%zu]='%s'\n", size, (char *)*buff);
	return size;
Exception:
	return -1;
}

/**
 	*@brief 发送数据：check revents & ZMQ_POLLOUT. 
	*@param timeout =-1 block, >=0 单位: ms.
	*@rcode -1，错误存储在errno中. >=0 size.
*/
static int32_t 
zmqmsg_send(IZeromq* self, void *buff, size_t size, long timeout){
	Assertor (self != NULL && buff != NULL);	
	zmq_msg_t msg;
	zmq_pollitem_t items = { 
		.socket = self->sock, .fd = 0, .events = ZMQ_POLLOUT, 
		.revents = 0 }; 
	TryCatch(-1 == (zmq_poll(&items, 1, timeout)),
		"zmqmsg_send failed! %s", zmq_strerror(errno));
	if (!(items.revents & ZMQ_POLLOUT))
		goto Exception;
	TryCatch((zmq_msg_init_size(&msg, size) == -1),
		"zmqmsg_send failed: %s", zmq_strerror(errno));
	memcpy(zmq_msg_data(&msg), buff, size);
	TryCatch((zmq_msg_send(&msg, self->sock, ZMQ_DONTWAIT) == -1),
		"zmqmsg_send failed: %s", zmq_strerror(errno));
	msg("@buff[%zu]='%s'\n", size, (char *)buff);
	return size;
Exception:
	zmq_msg_close(&msg);
	return -1;
}

/**
	 *@brief 接收数据：check revents & ZMQ_POLLIN. 
	 *@param buff, 
	 * ==NULL, need freed by caller.
	 * !=NULL, caller alloc.
	 *@param capa, !=NULL, ensure cap enough.
	 *@param timeout =-1 block, >=0 单位: ms.
	 *@rcode -1，错误存储在errno中. >=0 size.
*/
static int32_t 
zmqmsg_recv(IZeromq* self, void **pbuf, size_t capa, long timeout)
{
	Assertor (self != NULL && pbuf != NULL);	
    zmq_msg_t msg;
    zmq_pollitem_t items = { 
		.socket = self->sock, .fd = 0, .events = ZMQ_POLLIN, 
		.revents = 0 }; 
	TryCatch(-1 == (zmq_poll(&items, 1, timeout)),
		"zmqmsg_recv failed! %s", zmq_strerror(errno));
	if (!(items.revents & ZMQ_POLLIN)) {
		goto Exception;
	}
    TryCatch((zmq_msg_init(&msg) == -1),
		"zmqmsg_recv failed: %s", zmq_strerror(errno));
    TryCatch((zmq_msg_recv(&msg, self->sock, ZMQ_DONTWAIT) == -1),
		"zmqmsg_recv failed: %s", zmq_strerror(errno));
    size_t size = zmq_msg_size(&msg);
	if (*pbuf != NULL) {
		TryCatch(capa < size, "caller's buff not enough!");
	} else {
		*pbuf = calloc(1, size); // auto alloc .
	}
	Assertor (*pbuf != NULL);
    memcpy(*pbuf, zmq_msg_data(&msg), size);
    zmq_msg_close(&msg); // needed.
	msg("@buff[%zu]='%s'\n", size, (char *)*pbuf);
	return size;
Exception:
    zmq_msg_close(&msg);
	return -1;
}

// 封装socket通用类型.
static void
delete_socket(void** pptr){
	(void)pptr;
	return;
}

static void*
create_socket(void){
	return NULL;
}

#endif
/*******************************************************************************/

#endif //#define __SUNPF_UTIL__
