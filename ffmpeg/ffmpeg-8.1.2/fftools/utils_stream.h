
/*************************************************************************
 * Copyright (C), 1990-2020, Tech.Co., Ltd. All rights reserved.
 * @file   : utils_stream.h
 * @author : sun
 * @mail   : perfectsun1990@163.com 
 * @version: v1.0.0
 * @date   : 2018年05月20日 星期二 11时57分04秒 
 *-----------------------------------------------------------------------
 * @detail : 媒体模块
 * @         功能:加载系统依赖媒体接口、全局变量、宏及控制/通信协议.
 * gcc *.c -std=gnu11 [-std=c11 -D_XOPEN_SOURCE -D_XOPEN_SOURCE_EXTENDED ]
 * -lrt -lm -lc -lavcodec -lavfilter -lswscale -lswresample -lavutil
 ************************************************************************/
 
#pragma  once

#ifdef __cplusplus
extern "C" {
#endif

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"
#include "libavutil/timestamp.h"
#include "libavutil/avassert.h"
#include "libavutil/frame.h"

#include "libavdevice/avdevice.h"
#include "libavfilter/avfilter.h"
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "libavcodec/put_bits.h"

#ifdef 	__USESDL__
#include <SDL.h>
#include <SDL_thread.h>
#undef main //Note: SDL2 define main micro.
#endif

#ifdef __cplusplus
}
#endif


// Debugger assist functions.
static inline char*
av_str_err(int32_t ecode) {
	static __thread char strerr[512] = { 0 };
	memset(strerr, 0, sizeof(strerr));
	return (av_strerror(ecode, strerr, sizeof(strerr))) ? NULL : strerr;
}

static inline void
av_pkt_dbg(AVRational *time_base, const AVPacket *pkt, const char *tag) {
	printf("[PACKET]%s #stream-%d# (%d/%d): iskey=%d, pts:%ld pts_time=%0.6g(s) dts:%ld dts_time=%0.6g(s) duration=%ld, duration_time=%0.6g(ms)\n",
		tag, pkt->stream_index, time_base->num, time_base->den, pkt->flags,
		pkt->pts, pkt->pts*av_q2d(*time_base), pkt->dts, pkt->dts*av_q2d(*time_base), 
        pkt->duration, 1000 * pkt->duration*av_q2d(*time_base));
}

static inline void
av_frm_dbg(AVRational *time_base, const AVFrame  *frm, const char *tag) {
	bool is_audio = (frm->width == 0);
	printf("[FRAMES]%s #baudio-%d# (%d/%d): iskey=%d, pts:%ld pts_time=%0.6g(s) -->pkt_dts:%ld duration=%ld duration_time=%0.6g(ms)\n",
		tag, is_audio, time_base->num, time_base->den,
		!!(frm->flags & AV_FRAME_FLAG_KEY),
		frm->pts, frm->pts*av_q2d(*time_base), frm->pkt_dts,
		frm->duration, 1000 * frm->duration*av_q2d(*time_base));
}

// AVpackt operate functions


// AVFrame operate functions
static void
del_frame(AVFrame** frame) {
	av_frame_free(frame);
}

static AVFrame*
new_frame(int32_t format,int32_t width, int32_t height, uint64_t channel_layout_mask, int32_t sample_rate,
		int32_t nb_samples, int32_t align) {
	AVFrame *frame 				= av_frame_alloc();
	frame->format 				= format;
	frame->width  				= width;
	frame->height 				= height;
	frame->sample_rate			= sample_rate;
	frame->nb_samples			= nb_samples;
	if (channel_layout_mask)
		av_channel_layout_from_mask(&frame->ch_layout, channel_layout_mask);
	int32_t ret = av_frame_get_buffer(frame, align);
	av_assert0(ret == 0);
	return frame;
}

static AVFrame* 	//swr_convert_frame
resample(SwrContext **pswrctx, const AVFrame *src_frame, int32_t dst_format, int32_t dst_sample_rate, uint64_t dst_layout_mask) {
	av_assert0(NULL != pswrctx && NULL != src_frame);

	AVFrame* dst_frame 			= av_frame_alloc();
	dst_frame->format 			= dst_format;
	dst_frame->sample_rate		= dst_sample_rate;
	if (dst_layout_mask)
		av_channel_layout_from_mask(&dst_frame->ch_layout, dst_layout_mask);
	//dst_frame->nb_samples		= av_rescale_rnd(src_frame->nb_samples, dst_sample_rate, src_frame->sample_rate, AV_ROUND_UP);
	
	//	1.获取转换句柄.
	if (NULL == *pswrctx) {
		if (swr_alloc_set_opts2(pswrctx,
			&dst_frame->ch_layout, (enum AVSampleFormat)dst_frame->format, dst_frame->sample_rate,
			&src_frame->ch_layout, (enum AVSampleFormat)src_frame->format, src_frame->sample_rate,
			0, NULL) < 0)
			return NULL;
		if (swr_init(*pswrctx) < 0) {
			swr_free(pswrctx);
			return NULL;
		}
		printf("resampler is init!\n");
	}
	//	2.进行数据转换.
	int32_t ret = swr_convert_frame(*pswrctx, dst_frame, src_frame);
	if ( ret != 0 ) {
		av_log(NULL, AV_LOG_ERROR,"resampler error: %s, resampler is free!\n", av_str_err(ret));
		swr_free(pswrctx);
		av_frame_free(&dst_frame);
	}
	if (av_frame_copy_props(dst_frame, src_frame))
		return NULL;
	return dst_frame;
}

static inline char*
clone_pcm_to_data(AVFrame *frame, char* data, int32_t size) {
	if (NULL == frame || NULL == data || size <= 0)
		return NULL;
	memset(data, 0, size);
	int32_t bytes_per_sample = av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
	int32_t nb_channels = frame->ch_layout.nb_channels;
	int32_t frame_size = nb_channels * frame->nb_samples * bytes_per_sample;
	char*   frame_data = data, *p_cur_ptr = frame_data;

	if (frame_size > 0 && size >= frame_size) {// dump pcm
	 // 1.For packet sample foramt and 1 channel,we just store pcm data in byte order.
		if ((1 == nb_channels) || 0 == av_sample_fmt_is_planar((enum AVSampleFormat)frame->format))
		{//linesize[0] maybe 0 or has padding bits,so calculate the real size by ourself.
			memcpy(p_cur_ptr, frame->data[0], frame_size);
		}else {//2.For plane sample foramt, we must store pcm datas interleaved. [LRLRLR...LR].
			for (int32_t i = 0; i < frame->nb_samples; ++i) {
				memcpy(p_cur_ptr, frame->data[0] + i*bytes_per_sample, bytes_per_sample);
				p_cur_ptr += bytes_per_sample;
				memcpy(p_cur_ptr, frame->data[1] + i*bytes_per_sample, bytes_per_sample);
				p_cur_ptr += bytes_per_sample;
			}
		}
		return frame_data;
	}
	return NULL;
}

static inline void
pcm_freep(char* pcm_data) {
	if (NULL != pcm_data)
		free((void*)pcm_data);
}

static inline char*
pcm_clone(AVFrame *frame) {
	assert(NULL != frame);

	int32_t frame_size = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels, frame->nb_samples, (enum AVSampleFormat)frame->format, 1);
	char*	frame_data = (char*)calloc(1, frame_size);
	return clone_pcm_to_data(frame, frame_data, frame_size);
}

static inline void
Pcm_write(AVFrame *frame, const char* pfile, bool isend) {// Not Thread Safe!
	assert(NULL != frame);

	char name[128] = {};
	int32_t bytes_per_sample = av_get_bytes_per_sample((enum AVSampleFormat)frame->format);
	int32_t nb_channels = frame->ch_layout.nb_channels;
	sprintf(name, "./test_%dx%dx%dx%d_%ld.pcm",
		frame->sample_rate, nb_channels, frame->nb_samples, bytes_per_sample, time(NULL));
	if (pfile)
		sprintf(name, "%s", pfile);
	
	static FILE *fp = NULL;
	if (NULL == fp)
		fp = fopen(name, "wb+");
	if (NULL == fp)
		return;
#if 0
	int32_t pcm_size = av_samples_get_buffer_size(NULL, nb_channels, frame->nb_samples, (enum AVSampleFormat)frame->format, 1);
	char*   pcm_data = pcm_clone(frame);
	fwrite(pcm_data, 1, pcm_size, fp);
	pcm_freep(pcm_data);
#else
	// 1.For packet sample foramt and 1 channel,we just store pcm data in byte order.
	if ((1 == nb_channels) || 0 == av_sample_fmt_is_planar((enum AVSampleFormat)frame->format))
	{//linesize[0] maybe 0 or has padding bits,so calculate the real size by ourself.
		int32_t frame_size = nb_channels*frame->nb_samples*bytes_per_sample;
		fwrite(frame->data[0], 1, frame_size, fp);
	}
	else {//2.For plane sample foramt, we must store pcm datas interleaved. [LRLRLR...LR].
		for (int32_t i = 0; i < frame->nb_samples; ++i) {
			fwrite(frame->data[0] + i*bytes_per_sample, 1, bytes_per_sample, fp);
			fwrite(frame->data[1] + i*bytes_per_sample, 1, bytes_per_sample, fp);
		}
	}
#endif
	fflush(fp);
	if (isend && NULL != fp) {
		fclose(fp);
		fp = NULL;
	}
}

static AVFrame* 	//sws_scale frame
rescaled(struct SwsContext **pswsctx, AVFrame *src_frame, int32_t format, int32_t width, int32_t height) {
	av_assert0(NULL != pswsctx && NULL != src_frame);

	AVFrame *dst_frame = NULL;
	if (	src_frame->width  != width 
		||  src_frame->height != height
		||	src_frame->format != format )
	{	//  Auto call sws_freeContext() if reset.
		dst_frame = av_frame_alloc();
		av_assert0(dst_frame != NULL);
		dst_frame->width  = width;
		dst_frame->height = height;
		dst_frame->format = format;
		if (av_frame_get_buffer(dst_frame, 0) < 0)
			goto fail;
		//  1.获取转换句柄. [sws_getCachedContext will check cfg and reset *pswsctx]
		if (NULL == *pswsctx) {
			*pswsctx = sws_getContext(src_frame->width, src_frame->height, (enum AVPixelFormat)src_frame->format,
									  dst_frame->width, dst_frame->height, (enum AVPixelFormat)dst_frame->format,
									  0, NULL, NULL, NULL);
			av_log(NULL, AV_LOG_INFO,"@ New swsctx='%p'! \n", *pswsctx);
		}		
		av_assert0(*pswsctx != NULL);
		//  2.进行图像转换. [0-from begin]
		if (sws_scale(*pswsctx, 
			(const uint8_t* const*)src_frame->data, src_frame->linesize, 0, src_frame->height, 
				dst_frame->data, dst_frame->linesize) <= 0)
				goto fail;
	} else {
		dst_frame = av_frame_clone(src_frame);
		av_assert0(dst_frame != NULL);
	}
	dst_frame->pts = src_frame->pts;
	return dst_frame;
fail:
	av_frame_free(&dst_frame);
	return NULL;
}

static inline char*
clone_yuv_to_data(AVFrame *frame, char* data, int32_t size) {
	if (NULL == frame || NULL == data || size <= 0)
		return NULL;

	memset(data, 0, size);
	int32_t y_size = frame->width * frame->height;
	int32_t frame_size =  (frame->format == AV_PIX_FMT_YUVA420P) ? y_size * 5 / 2:y_size * 3 / 2;
    //msg("total size=%d ,%d\n",frame_size, av_image_get_buffer_size(frame->format, frame->width, frame->height, 1));
	char*   frame_data = data, *p_cur_ptr = frame_data;
	if (frame_size > 0 && size >= frame_size) {// dump yuv420
		for (int32_t i = 0; i < frame->height; ++i)
			memcpy(p_cur_ptr + i*frame->width    , frame->data[0] + frame->linesize[0] * i, frame->width);
		p_cur_ptr += y_size;
		for (int32_t i = 0; i < frame->height / 2; ++i)
			memcpy(p_cur_ptr + i*frame->width / 2, frame->data[1] + frame->linesize[1] * i, frame->width / 2);
		p_cur_ptr += y_size / 4;
		for (int32_t i = 0; i < frame->height / 2; ++i)
			memcpy(p_cur_ptr + i*frame->width / 2, frame->data[2] + frame->linesize[2] * i, frame->width / 2);
		if (frame->format == AV_PIX_FMT_YUVA420P) {
			for (int32_t i = 0; i < frame->height; ++i)
				memcpy(p_cur_ptr + i*frame->width, frame->data[3] + frame->linesize[3] * i, frame->width);
		}
		return frame_data;
	}
	return NULL;
}

static inline void
yuv_freep(char* yuv_data) {
	free((void*)yuv_data);
}
//Note: only support yuv420.
static inline char*
yuv_clone(AVFrame *frame) {
	av_assert0(NULL != frame);
	
	int32_t frame_size = av_image_get_buffer_size((enum AVPixelFormat)frame->format, frame->width, frame->height, 1);
	char*	frame_data = (char*)calloc(1, frame_size);
	return clone_yuv_to_data(frame, frame_data, frame_size);
}

static inline void
yuv_write(AVFrame *frame, const char* pfile, bool isend) {
	av_assert0(NULL != frame);// Not Thread Safe!
    
    static struct SwsContext *swsctx = NULL;
    AVFrame* ptmp = NULL, *pfrm = frame;
    if (pfrm->format != AV_PIX_FMT_YUV420P && pfrm->format != AV_PIX_FMT_YUVA420P) {
        ptmp = rescaled(&swsctx, pfrm, AV_PIX_FMT_YUV420P, pfrm->width, pfrm->height);
        if ( NULL != ptmp) {
            av_log(NULL, AV_LOG_ERROR,"rescaled failed! ptmp=%p\n", ptmp);
            del_frame(&ptmp);
            return;
        }
        pfrm = ptmp;//write converted frame.
        av_log(NULL, AV_LOG_INFO,"convert to i420!%dx%d\n", ptmp->width, ptmp->height);
    }
	char name[512] = { 0 };
	sprintf(name, "./test_%dx%dx%d_%ld.yuv", 
        pfrm->width, pfrm->height,(int32_t)pfrm->format,time(NULL));
	if (pfile)
		sprintf(name, "%s", pfile);
	static FILE *fp = NULL;
	if (NULL == fp)
		fp = fopen(name, "wb+");
	if (NULL == fp)
		return;
#if 0//剥离AVFrame中的裸数据。	
    int32_t yuv420_size = av_image_get_buffer_size(pfrm->format, pfrm->width, pfrm->height, 1);
    char*   yuv420_data = yuv_clone(pfrm);
	fwrite(yuv420_data, 1, yuv420_size, fp);
	yuv_freep(yuv420_data);
#else
	//write yuv420p(I420)
	for (int32_t i = 0; i < pfrm->height; ++i)
		fwrite(pfrm->data[0] + pfrm->linesize[0] * i, 1, pfrm->width, fp);
	for (int32_t i = 0; i < pfrm->height / 2; ++i)
		fwrite(pfrm->data[1] + pfrm->linesize[1] * i, 1, pfrm->width / 2, fp);
	for (int32_t i = 0; i < pfrm->height / 2; ++i)
		fwrite(pfrm->data[2] + pfrm->linesize[2] * i, 1, pfrm->width / 2, fp);
	if (pfrm->format == AV_PIX_FMT_YUVA420P) {
		for (int32_t i = 0; i < pfrm->height; ++i)
			fwrite(pfrm->data[3] + pfrm->linesize[3] * i, 1, pfrm->width, fp);
	}
#endif  
    fflush(fp);
	if (isend && NULL != fp) {
		fclose(fp);
		fp = NULL;
        sws_freeContext(swsctx);
	}
    del_frame(&ptmp);
}


// NALU 类型描述数组，使用 FFmpeg 的定义
static const char *nalu_description[] = {
    "Unspecified",           // 0
    "Coded slice of a non-IDR picture",       // 1
    "Coded slice data partition A",           // 2
    "Coded slice data partition B",           // 3
    "Coded slice data partition C",           // 4
    "Coded slice of an IDR picture",          // 5
    "Supplemental enhancement information (SEI)", // 6
    "Sequence parameter set",                 // 7
    "Picture parameter set",                  // 8
    "Access unit delimiter",                  // 9
    "End of sequence",                        // 10
    "End of stream",                          // 11
    "Filler data",                            // 12
    "Sequence parameter set extension",       // 13
    "Prefix NAL unit",                        // 14
    "Subset sequence parameter set",          // 15
    "Reserved",                               // 16
    "Reserved",                               // 17
    "Reserved",                               // 18
    "Coded slice of an auxiliary coded picture without partitioning", // 19
    "Coded slice extension",                  // 20
    "Coded slice extension for a depth view or 3D-AVC texture picture", // 21
    "Reserved",                               // 22
    "Reserved",                               // 23
    "Unspecified",                            // 24
    "Unspecified",                            // 25
    "Unspecified",                            // 26
    "Unspecified",                            // 27
    "Unspecified",                            // 28
    "Unspecified",                            // 29
    "Unspecified",                            // 30
    "Unspecified",                            // 31
};

// 根据 NALU 类型返回描述字符串
static const char* get_nalu_description(int32_t codec_type, uint8_t nalu_type) {
	switch (codec_type) {
		case 0: // 264.
		    if (nalu_type < sizeof(nalu_description) / sizeof(nalu_description[0])) {
		        return nalu_description[nalu_type];
		    }
			break;
		case 1: // 265.
			break;
		case 2: // av1.
			break;			
	}
    return "Unknown NALU type";
}

/* Convert EBSP (Encapsulated Byte Sequence Packets) to RBSP (Raw Byte Sequence Packets). */
static uint8_t *nalu_remove_emulation_prevention(    uint8_t *src,    uint64_t src_length,    uint8_t *dst) {
    uint8_t *src_end = src + src_length;
    while( src < src_end )
        if( ((src + 2) < src_end) && !src[0] && !src[1] && (src[2] == 0x03) )
        {
            /* 0x000003 -> 0x0000 */
            *dst++ = *src++;
            *dst++ = *src++;
            src++;  /* Skip emulation_prevention_three_byte (0x03). */
        }
        else
            *dst++ = *src++;
    return dst;
}

/*!
************************************************************************
*  \brief
*     This function add emulation_prevention_three_byte for all occurrences
*     of the following byte sequences in the stream
*       0x000000  -> 0x00000300
*       0x000001  -> 0x00000301
*       0x000002  -> 0x00000302
*       0x000003  -> 0x00000303
*
*  \param ebsp
*            pointer to target buffer
*  \param rbsp
*            pointer to source buffer
*  \param rbsp_size
*           Size of source
*  \return
*           Size target buffer after emulation prevention.
*
************************************************************************
*/
#define ZEROBYTES_SHORTSTARTCODE 2
static uint32_t 
rbsp_to_ebsp(unsigned char *ebsp, unsigned char *rbsp, uint32_t rbsp_size) {
  uint32_t j     = 0;
  uint32_t count = 0;
  uint32_t i;

  for(i = 0; i < rbsp_size; i++)
  {
    if(count == ZEROBYTES_SHORTSTARTCODE && !(rbsp[i] & 0xFC))
    {
      ebsp[j] = 0x03;
      j++;
      count = 0;
    }
    ebsp[j] = rbsp[i];
    if(rbsp[i] == 0x00)
      count++;
    else
      count = 0;
    j++;
  }

  return j;
}

//len = RBSPtoEBSP (nalu->buf, rbsp, rbsp_size)
static int 
nalu_import_rbsp_from_ebsp(    uint8_t    *rbsp_buffer, uint64_t *rbsp_size, uint8_t *ebsp, uint64_t ebsp_size) {
    uint8_t *rbsp_start = rbsp_buffer;
    uint8_t *rbsp_end   = nalu_remove_emulation_prevention( ebsp, ebsp_size, rbsp_buffer );
    *rbsp_size = (uint64_t)(rbsp_end - rbsp_start);
    if( *rbsp_size > ebsp_size )
        return -1;
    return 0;
}

//SEI NALU: 0x06(1)+0x05(1)+[payload_size=uuid_size+data_size+trailing](1-n)+uuid(16)+user_data(data_size)+trailing bytes(1-2)
static inline uint32_t 
get_sei_nalu_size(bool is_hevc, int32_t uuid_size, uint32_t content_size)
{
	uint32_t nalu_hdr_size = is_hevc ? 2 : 1;
	
	// sei_payload_size = content_size + uuid_size
	uint32_t sei_payload_size = content_size + uuid_size; // eg. get_uuid_size(app_type);
	// rbsp_size = sei_payload_type_size(1) +  sei_payload_size_size(n字节) + sei_payload_size + tail_size)
	uint32_t rbsp_size = 1 + (sei_payload_size / 0xFF + (sei_payload_size % 0xFF != 0 ? 1 : 0)) + sei_payload_size;
	// nalu_size = nalu_header_size(1byte) + rbsp_size.
	uint32_t nalu_size = nalu_hdr_size + rbsp_size;
	// 设置(1/2)rbsp截止码(0x80)	
	nalu_size += 1; //非字符串类型，直接跟0x80即可.	
	// nalu_size += ((nalu_size % 2 == 1) ? 1 : 2);
	return nalu_size;
}

static inline uint32_t
reversebytes(uint32_t value) {
	return (value & 0x000000FFU) << 24 | (value & 0x0000FF00U) << 8 |
		(value & 0x00FF0000U) >> 8 | (value & 0xFF000000U) >> 24;
}

// 拷贝时SEI包时，忽略[]uuid/uuid_size/payload_type]三个字段；
static uint32_t 
build_sei_nalu_packet_copy(unsigned char* uuid, int32_t uuid_size, unsigned char payload_type, bool is_hevc, bool isAnnexb, 
	unsigned char* nalu_data_out, uint32_t nalu_data_out_size, const char * content, uint32_t content_size) {
	const uint8_t start_code[] = {0x00,0x00,0x00,0x01};
	// 计算nalu需要的空间大小, nalu_size = nalu_header_size(1byte) + rbsp_size(rbsp(sodb+trailing bits)) 	
	uint32_t nalu_hdr_size = is_hevc ? 2 : 1;
	uint32_t nalu_size = nalu_hdr_size + content_size; // 拷贝原始SEI-NALU信息：剥离掉原始格式SEI头, 按输出格式重新加SEI头.
	// uint32_t nalu_size = get_sei_nalu_size(is_hevc, uuid_size, content_size);//TODO:motify.
	uint32_t nalu_size_with_mark = nalu_size + sizeof(uint32_t);
	if (nalu_data_out_size < nalu_size_with_mark) {
		err("Hi! not enough nalu space =%d, at least > %d\n", nalu_size_with_mark, nalu_size_with_mark);
		return -1;
	}
	uint32_t big_endian_nalu_size = reversebytes(nalu_size);
	// NALU 的开始码或者长度.
	if (isAnnexb) {
		memcpy(nalu_data_out, start_code, sizeof(unsigned int));
	}else {
		memcpy(nalu_data_out, &big_endian_nalu_size, sizeof(unsigned int));
	}
	nalu_data_out +=  sizeof(uint32_t);//startcode.
	if (is_hevc) { //int type = (code & 0x7E)>>1; 39
		*nalu_data_out++ = 0x4E;	//SEI nalu_header.
		*nalu_data_out++ = 0x01;	//SEI nalu_header.
	} else {
		*nalu_data_out++ = 0x06;	//SEI nalu_header.
	}
	memcpy(nalu_data_out, content, content_size);
	return nalu_size_with_mark;
}

static void put_leb128(PutBitContext *pb, uint32_t value) {
	while (value > 127) {
		put_bits(pb, 8, (value & 0x7F) | 0x80); // 输出最低7位，并将MSB设为1
		value >>= 7;
	}
	put_bits(pb, 8, value & 0x7F); // 最后一个字节，MSB为0
}

// #include <libavcodec/put_bits.h>
// AV1-OBU-syntax:
// OBU = [obu-header + obu-size(leb) + obu-payload]. obu-payload-
// metadata_obu = metadata_type <public:[0,1-5,32+], private:[6-31]>
// eg. uchar data[] = { 0x2A, 0x04, 0x06, 0x02, 0x03, 0x04};
static void put_obu_metadata(PutBitContext *pb, bool isAnnexb, const uint8_t *payload, size_t payload_size) {
	if (isAnnexb) {
		av_log(NULL, AV_LOG_DEBUG, "Hi! we just ignore av1 isAnnexb's format currently!");
	}
	// fill the OBU header for a metadata type OBU
	uint32_t obu_forbidden_bit = 0;
	uint32_t obu_type = 5;	// OBU_TYPE_METADATA
	uint32_t obu_extension_flag = 0;
	uint32_t obu_has_size_field = 1;
	uint32_t obu_reserved_1bit = 0;
	
	// fill obu_header fields
	put_bits(pb, 1, obu_forbidden_bit); 	// 1 bit
	put_bits(pb, 4, obu_type);				// 4 bits
	put_bits(pb, 1, obu_extension_flag);	// 1 bit
	put_bits(pb, 1, obu_has_size_field);	// 1 bit
	put_bits(pb, 1, obu_reserved_1bit); 	// 1 bit
	// fill obu_size field
	// obu_size contains the size in bytes of the OBU not including the bytes within obu_header or the obu_size syntax element.
    uint32_t obu_size = payload_size + sizeof(char);
	if (obu_has_size_field) {
		put_leb128(pb, obu_size);
	}
	// Fill obu_data-metadata_payload_type
	uint32_t metadata_type = 0x06;
	put_bits(pb, 8, metadata_type);			// 8 bits, <public:[0,1-5,32+], private:[6-31]>
	// Fill obu_data-metadata_payload_size (user data)
	for (size_t i = 0; i < payload_size; i++) {
		put_bits(pb, 8, payload[i]);
	}
}

// ；
static uint32_t 
build_av1_obu_metadata(bool isAnnexb, unsigned char* nalu_data_out, uint32_t nalu_data_out_size, const char * content, uint32_t content_size) {

	// check buffer.
	if (nalu_data_out_size < content_size + 1 + 4) {
		av_log(NULL, AV_LOG_ERROR, "Hi! not enough nalu space =%d, at least > %d\n", nalu_data_out_size, content_size+6);
		return 0;
	}

    PutBitContext pb;
    init_put_bits(&pb, nalu_data_out, nalu_data_out_size);
	// Create OBU metadata bitstream, OBU Size=Payload size
    put_obu_metadata(&pb, isAnnexb, (const uint8_t *)content, content_size);
	align_put_bits(&pb);
    flush_put_bits(&pb);	
	int32_t real_bits = put_bits_count(&pb);
	nalu_data_out_size = (real_bits+7)/8;

	av_log(NULL, AV_LOG_INFO, "@@@@ build real_bits=%d, nalu_data_out_size=%d\n", real_bits, nalu_data_out_size);
	return nalu_data_out_size;
}

// 确保nalu_data_out_size足够大哦。
// Warnning:返回（nalu单元+startcode）的大小。
static uint32_t 
build_sei_nalu_packet(unsigned char* uuid, int32_t uuid_size, unsigned char payload_type, bool is_hevc, bool isAnnexb, 
	unsigned char* nalu_data_out, uint32_t nalu_data_out_size, const char * content, uint32_t content_size)
{
	const uint8_t start_code[] = {0x00,0x00,0x00,0x01};

	uint32_t nalu_hdr_size = is_hevc ? 2 : 1;
	//int32_t uuid_size = get_uuid_size(app_type);
	
	// 计算nalu需要的空间大小, nalu_size = nalu_header_size(1byte) + rbsp_size(rbsp(sodb+trailing bits)) 
	uint32_t		nalu_size = get_sei_nalu_size(is_hevc, uuid_size, content_size);
	unsigned char* 	nalu_data = (unsigned char*)calloc(1, nalu_size);
	unsigned char* 	ptr = nalu_data;	//临时指针.
	
	// 计算负载大小和存储改大小需要的字节数.
	uint32_t sei_payload_size = uuid_size + content_size;
	uint32_t sei_payload_size_bytes = (sei_payload_size / 0xFF + (sei_payload_size % 0xFF != 0 ? 1 : 0));	
	// ptr   指向NALU的起始字节. ptr指向 nalu header.
	// ptr+1 指向 nalu body(RBSP).
	// ptr+offset_from_nalu_start 指向rbsp除去了:sei type(1) + sei payload_size.(n)的位置.
	uint32_t offset_from_nalu_start = nalu_hdr_size + 1 + sei_payload_size_bytes;	
	ptr += offset_from_nalu_start;
	unsigned char*	rbsb_payload_data	= ptr;// 指向UUID, 转换为 ebsp 的 rbsp 数据起始地址.
	uint32_t 		rbsb_payload_size  	= nalu_size-offset_from_nalu_start;	//待处理rbsp_payload的大小
	
	if (NULL != uuid && uuid_size > 0) { // 填充用户UUID。
		memcpy(ptr, uuid, uuid_size);
		ptr += uuid_size;
	}
	// 填充用户数据。
	memcpy(ptr, content, content_size);
	ptr += content_size;
	// 填充对齐码值。(rbsp trailing bits/NAL unit结尾写入的字节一定是0x80) 
	if (nalu_data + nalu_size - ptr == 1) {
            *ptr++ = 0x80;
	} else if (nalu_data + nalu_size - ptr == 2) {
           // *ptr++ = 0x00;
            *ptr++ = 0x80;
	}
	
	// 进行EBSB编码。	
	unsigned char* 	ebsp_payload_data = (unsigned char*)calloc(1, 2*rbsb_payload_size); //申请足够存储转换后的ebsp的缓存.
	uint32_t 		ebsp_payload_size = rbsp_to_ebsp(ebsp_payload_data, rbsb_payload_data, rbsb_payload_size);
	av_log(NULL, AV_LOG_DEBUG, "Hi! >EBSP encode>>> nalu_data_out_size=%d, ebsp_payload_size=%d, rbsb_payload_size=%d\n", 
					nalu_data_out_size, ebsp_payload_size, rbsb_payload_size);
	if (nalu_data_out_size < ebsp_payload_size) {
		av_log(NULL, AV_LOG_ERROR, "Hi! not enough nalu space =%d, at least > %d\n", nalu_data_out_size, ebsp_payload_size);
		return 0;
	}
	
	// [#编码NALU单元#] 重新计算ebsp编码后的：nalu_size,只要加上增量即可,
	// sei_payload_size += (ebsp_payload_size - rbsb_payload_size);//将0x03计入负载?不需要，因为解码时不会修改负载大小。
	// sei_payload_size_bytes = (sei_payload_size / 0xFF + (sei_payload_size % 0xFF != 0 ? 1 : 0));
	nalu_size += (ebsp_payload_size - rbsb_payload_size);
	// 填充Annex B格式或者AVCC格式的nalu起始字节。
	// 注意：进行字节序转换，通常是小端转大端. (低地址存储高字节，高地址存储低字节)	
	uint32_t big_endian_nalu_size = reversebytes(nalu_size);
	//NALU 的开始码或者长度.
	if (isAnnexb) {
		memcpy(nalu_data_out, start_code, sizeof(unsigned int));
	}else {
		memcpy(nalu_data_out, &big_endian_nalu_size, sizeof(unsigned int));
	}
	nalu_data_out +=  sizeof(uint32_t);//startcode.
	if (is_hevc) { // int type = (code & 0x7E)>>1; 39
		*nalu_data_out++ = 0x4E;	//SEI nalu_header.
		*nalu_data_out++ = 0x01;	//SEI nalu_header.
	} else {
		*nalu_data_out++ = 0x06;	//SEI nalu_header.
	}
	
	// SEI payload type, default: 0x05, 可定制.
	*nalu_data_out++ = payload_type; 
	
	while (true) {		 	//sei payload size, 数据长度. (不包含对齐的0x80)
		*nalu_data_out++ = (sei_payload_size >= 0xFF ? 0xFF : (char)sei_payload_size);
		if (sei_payload_size < 0xFF) break;
		sei_payload_size 	-= 0xFF;
	}
	memcpy(nalu_data_out, ebsp_payload_data, ebsp_payload_size);

	// 获取实际大小。
	nalu_data_out_size = nalu_size + sizeof(uint32_t);
	av_log(NULL, AV_LOG_DEBUG, "Hi! >EBSP encode>>> nalu_data_out_size=%d, ebsp_payload_size=%d, rbsb_payload_size=%d\n", 
					nalu_data_out_size, ebsp_payload_size, rbsb_payload_size);
	free(nalu_data);
	free(ebsp_payload_data);
	return nalu_data_out_size;
}


typedef struct AVFilterGraphWrap {
	AVFilterGraph	*graph; 
	AVFilterContext *buffersrc_ctx;
	AVFilterContext *buffersik_ctx; 
	AVFilterInOut	*buffersrc_outputs;
	AVFilterInOut	*buffersink_inputs;
	// gpu.
	AVBufferRef 	*hw_device_ref;
	AVBufferRef 	*src_hw_frames_ref;
	AVBufferRef 	*dst_hw_frames_ref;
	// tmp.
	AVFrame 		*src_tmp_frame;
	AVFrame 		*dst_tmp_frame;
} AVFilterGraphWrap;

static AVBufferRef* 
hw_frames_ref(AVBufferRef *device_ctx, enum AVPixelFormat format, int width, int height)
{
	AVBufferRef *out_ref = NULL;
	AVHWFramesContext *out_ctx;
	int32_t ret = -1;
	out_ref = av_hwframe_ctx_alloc(device_ctx);
	if (!out_ref)
		return NULL;
	out_ctx = (AVHWFramesContext*)out_ref->data;
	out_ctx->format    = AV_PIX_FMT_CUDA;
	out_ctx->sw_format = format;
	out_ctx->width	   = FFALIGN(width,  32);
	out_ctx->height    = FFALIGN(height, 32);
	ret = av_hwframe_ctx_init(out_ref);
	if (ret < 0)
		goto fail;
	return out_ref;
fail:
	av_buffer_unref(&out_ref);
	return NULL;
}

static void
delete_fg_wrap(AVFilterGraphWrap **p_wrap) {
	if (NULL == p_wrap || NULL == *p_wrap)
		return ;
	AVFilterGraphWrap *filter_graph_wrap = *p_wrap;
	av_frame_free(&filter_graph_wrap->src_tmp_frame);
	av_frame_free(&filter_graph_wrap->dst_tmp_frame);
	av_buffer_unref(&filter_graph_wrap->src_hw_frames_ref);
	av_buffer_unref(&filter_graph_wrap->dst_hw_frames_ref);
	av_buffer_unref(&filter_graph_wrap->hw_device_ref);
	avfilter_inout_free(&filter_graph_wrap->buffersink_inputs);
	avfilter_inout_free(&filter_graph_wrap->buffersrc_outputs);
	avfilter_graph_free(&filter_graph_wrap->graph);
	av_freep(&filter_graph_wrap);
	*p_wrap = NULL;
}

// graph_desc: 除出入口filter外的用户滤镜链路.
// src_hw_frames_ref和dst_hw_frames_ref均为NULL时, 代表纯CPU滤镜链路.
// 否则，请保持和graph_desc搭配正确，避免错误.
static AVFilterGraphWrap *
create_fg_wrap(int32_t use_gpu, AVRational src_tb, AVRational src_sar
	, AVBufferRef *src_hw_frames_ref, int32_t src_fmt, int32_t src_width, int32_t src_height
	, AVBufferRef *dst_hw_frames_ref, int32_t dst_fmt, int32_t dst_width, int32_t dst_height, const char* graph_desc) {
	
	AVFilterGraphWrap *filter_graph_wrap = av_mallocz(sizeof(AVFilterGraphWrap));
	filter_graph_wrap->graph = avfilter_graph_alloc();
	filter_graph_wrap->buffersink_inputs = avfilter_inout_alloc();
	filter_graph_wrap->buffersrc_outputs = avfilter_inout_alloc();
	int32_t buffer_src_fmt = src_fmt;
	int ret = -1;
	
	if (use_gpu) {
		// (1) hw_device_ref
		if (NULL == src_hw_frames_ref && NULL == dst_hw_frames_ref) { // try init a new GPU hw_device.
			int32_t gpu_device_id = 0; // TODO: 可选的参数.
			char device[64];
			snprintf(device, sizeof(device), "%d", gpu_device_id);
			ret = av_hwdevice_ctx_create(&filter_graph_wrap->hw_device_ref, AV_HWDEVICE_TYPE_CUDA, device, NULL, 0);
			if (ret <0) goto fail;
			buffer_src_fmt = AV_PIX_FMT_CUDA;		 // eg. cuda...
		} else {  // try reuse a old GPU hw_device.
			AVHWFramesContext *hw_frames_ctx  = src_hw_frames_ref ? \
				(AVHWFramesContext*)src_hw_frames_ref->data : (AVHWFramesContext*)dst_hw_frames_ref->data;
			filter_graph_wrap->hw_device_ref  = av_buffer_ref(hw_frames_ctx->device_ref);
			buffer_src_fmt =  hw_frames_ctx->format; // eg. cuda...
		}
		
		// (2) hw_frames_ref
		if (NULL == src_hw_frames_ref) {
			filter_graph_wrap->src_hw_frames_ref = hw_frames_ref(filter_graph_wrap->hw_device_ref, src_fmt, src_width, src_height);
			if (!filter_graph_wrap->src_hw_frames_ref) 
				goto fail;
		} else {
			filter_graph_wrap->src_hw_frames_ref = av_buffer_ref(src_hw_frames_ref); // 复用src的hw_frames_ref上下文.
		}
		filter_graph_wrap->src_tmp_frame = av_frame_alloc();
		av_frame_unref(filter_graph_wrap->src_tmp_frame);
		ret = av_hwframe_get_buffer(filter_graph_wrap->src_hw_frames_ref, filter_graph_wrap->src_tmp_frame, 0);
		if (ret < 0) goto fail;
		filter_graph_wrap->src_tmp_frame->width  = src_width;
		filter_graph_wrap->src_tmp_frame->height = src_height;
		
		if (NULL == dst_hw_frames_ref) {
			filter_graph_wrap->dst_hw_frames_ref = hw_frames_ref(filter_graph_wrap->hw_device_ref, dst_fmt, dst_width, dst_height);
			if (!filter_graph_wrap->dst_hw_frames_ref) 
				goto fail;
		} else {
			filter_graph_wrap->dst_hw_frames_ref = av_buffer_ref(dst_hw_frames_ref); // 复用dst的hw_frames_ref上下文.
		}
		filter_graph_wrap->dst_tmp_frame = av_frame_alloc();
		av_frame_unref(filter_graph_wrap->dst_tmp_frame);
		ret = av_hwframe_get_buffer(filter_graph_wrap->src_hw_frames_ref, filter_graph_wrap->dst_tmp_frame, 0);
		if (ret < 0) goto fail;
		filter_graph_wrap->dst_tmp_frame->width  = dst_width;
		filter_graph_wrap->dst_tmp_frame->height = dst_height;
		
	} else {
		av_log(NULL, AV_LOG_INFO, "@filter_graph use_gpu=%d, don't try gpu filters!", use_gpu);
	}
	
	/* (1).滤镜链路的出入口.*/
	char args[512] = {0}; // 输如到filter帧的codec参数.
	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		src_width, src_height, buffer_src_fmt, src_tb.num, src_tb.den, src_sar.num, src_sar.den);
	avfilter_graph_create_filter(&filter_graph_wrap->buffersrc_ctx , avfilter_get_by_name("buffer"),
								"in", args, NULL, filter_graph_wrap->graph);
	
	AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
	av_assert0(NULL != par);
	if (use_gpu) {// Set the GPU frames context for buffersrc
		par->hw_frames_ctx = av_buffer_ref(filter_graph_wrap->src_hw_frames_ref); // GPU.
	}
	if (av_buffersrc_parameters_set(filter_graph_wrap->buffersrc_ctx, par) < 0)
		goto fail;
	av_freep(&par);
	av_log(NULL, AV_LOG_INFO, "@buffersrc setup ok! use_gpu=%d, buffersrc->args=%s\n", use_gpu, args);
	
	// sink 无需配置，自动生成. 
	ret = avfilter_graph_create_filter(&filter_graph_wrap->buffersik_ctx, avfilter_get_by_name("buffersink"),
								"out", NULL, NULL, filter_graph_wrap->graph);
	if (ret < 0) goto fail;
	
	/* (2).配置滤镜处理链路. */
	// outputs变量意指buffersrc_ctx滤镜的输出引脚(output pad)
	// src缓冲区(buffersrc_ctx滤镜)的输出必须连到filters_descr中第一个
	// 滤镜的输入；filters_descr中第一个滤镜的输入标号未指定，故默认为
	// "in"，此处将buffersrc_ctx的输出标号也设为"in"，就实现了同标号相连
	filter_graph_wrap->buffersrc_outputs->name		= av_strdup("in");
	filter_graph_wrap->buffersrc_outputs->filter_ctx = filter_graph_wrap->buffersrc_ctx; //buffersrc_ctx
	filter_graph_wrap->buffersrc_outputs->pad_idx	= 0;
	filter_graph_wrap->buffersrc_outputs->next		= NULL;
	// inputs变量意指buffersink_ctx滤镜的输入引脚(input pad)
	// sink缓冲区(buffersink_ctx滤镜)的输入必须连到filters_descr中最后
	// 一个滤镜的输出；filters_descr中最后一个滤镜的输出标号未指定，故
	// 默认为"out"，此处将buffersink_ctx的输出标号也设为"out"，就实现了
	// 同标号相连(准确的应该叫替换标号fitler)
	filter_graph_wrap->buffersink_inputs->name	   = av_strdup("out");
	filter_graph_wrap->buffersink_inputs->filter_ctx = filter_graph_wrap->buffersik_ctx; //bufferout_ctx 
	filter_graph_wrap->buffersink_inputs->pad_idx	 = 0;
	filter_graph_wrap->buffersink_inputs->next	   = NULL;
	// 将filters_descr描述的滤镜图添加到filter_graph滤镜图中
	// 调用前：filter_graph包含两个滤镜buffersrc_ctx和buffersink_ctx
	// 调用后：filters_descr描述的滤镜图插入到filter_graph中，buffersrc_ctx连接到
	//		  filters_descr的输入，filters_descr的输出连接到buffersink_ctx，
	//		  filters_desc只进行了解析而不建立内部滤镜间的连接。
	//		  filters_desc与filter_graph间的连接是利用AVFilterInOut inputs
	//		  和AVFilterInOut outputs连接起来的，AVFilterInOut是一个链表，
	//		  最终可用的连在一起的滤镜链/滤镜图就是通过这个链表串在一起的。
	ret = avfilter_graph_parse_ptr(filter_graph_wrap->graph, graph_desc,
		&filter_graph_wrap->buffersink_inputs, &filter_graph_wrap->buffersrc_outputs, NULL);
	if (ret < 0) goto fail;
	
	/* (3).构建滤镜处理链路.*/
	ret = avfilter_graph_config(filter_graph_wrap->graph, NULL);
	if (ret < 0) goto fail;
	
	return filter_graph_wrap;
	
fail:
	av_freep(&par);
	delete_fg_wrap(&filter_graph_wrap);
	return NULL;
}

// eg. rescale cuda.
static AVFrame* 
fg_scaled_cuda(AVFilterGraphWrap **p_wrap, AVRational src_tb, AVFrame *src, int32_t dst_fmt, int32_t dst_width, int32_t dst_height) {
	if (AV_PIX_FMT_CUDA != src->format || NULL == src->hw_frames_ctx) {
		av_log(NULL, AV_LOG_ERROR, "Unsupport GPU format=%s or src->hw_frames_ctx=NULL.\n", av_get_pix_fmt_name(src->format));
		return NULL;
	}
    AVHWFramesContext *hw_frames_ctx = (AVHWFramesContext*)src->hw_frames_ctx->data;
	AVFilterGraphWrap *filter_graph_wrap = *p_wrap;

	if (NULL == filter_graph_wrap) {
		char graph_desc[512] = {0};
		snprintf(graph_desc, sizeof(graph_desc),"scale_cuda=%d:%d:format=%s", dst_width, dst_height, av_get_pix_fmt_name(dst_fmt));
		filter_graph_wrap = *p_wrap = create_fg_wrap(1, src_tb, src->sample_aspect_ratio, 
				src->hw_frames_ctx, src->format, src->width, src->height, 
				src->hw_frames_ctx, dst_fmt, dst_width, dst_height, graph_desc);
		av_assert0(NULL != filter_graph_wrap);
		av_log(NULL, AV_LOG_INFO, "@[GPU] fp_wrap=%p, covert: '%d:%d:%s(%s)'-> '%d:%d:%s'\n" 
			, filter_graph_wrap, src->width, src->height, av_get_pix_fmt_name(src->format) , av_get_pix_fmt_name(hw_frames_ctx->sw_format)
			, dst_width, dst_height, av_get_pix_fmt_name(dst_fmt));
	}

	// 执行滤镜链动作.
#if 1
	av_frame_unref(filter_graph_wrap->src_tmp_frame);
	filter_graph_wrap->src_tmp_frame = av_frame_clone(src);
#else
    ret = av_hwframe_get_buffer(src->hw_frames_ctx, filter_graph_wrap->src_tmp_frame, 0);
    if (ret < 0)
        return ret;
    // 复制GPU帧数据
    if (av_hwframe_transfer_data(filter_graph_wrap->src_tmp_frame, src, 0) < 0) {
		return NULL;
    }
	filter_graph_wrap->src_tmp_frame->width  = src->width;
	filter_graph_wrap->src_tmp_frame->height = src->height;
    ret = av_frame_copy_props(filter_graph_wrap->src_tmp_frame, src);
#endif
	if (av_buffersrc_add_frame(filter_graph_wrap->buffersrc_ctx, filter_graph_wrap->src_tmp_frame) < 0) {
		av_frame_unref(filter_graph_wrap->src_tmp_frame);
		return NULL;
	}	
	av_frame_unref(filter_graph_wrap->src_tmp_frame);
	
	/* pull filtered pictures from the filtergraph */
	AVFrame *dst = av_frame_alloc();
#if 0
    ret = av_hwframe_get_buffer(filter_graph_wrap->dst_hw_frames_ref, dst, 0);
    if (ret < 0)
        return ret;
	dst->hw_frames_ctx = filter_graph_wrap->dst_hw_frames_ref;
	dst->width  = dst_width;
	dst->height = dst_width;
#endif
	if (av_buffersink_get_frame(filter_graph_wrap->buffersik_ctx, dst) < 0) {
		av_frame_free(&dst);
		return NULL;
	}
	return dst;
		
}

