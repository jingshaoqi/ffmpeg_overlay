/**  ffmpeg_overlay_1.c
* ffmpeg version:3.2.4
*本程序可以在windows上(最好用VS2013)和linux系统上运行
*最简单的基于FFmpeg的添加水印（叠加图片到视频上）的程序
*本程序主要实现从文件中提取视频频，解码后经过叠加图片，最后压缩为264的ES流并保存到文件
*
*gcc -g -o ffmpeg_overlay_1 ffmpeg_overlay_1.c -lavfilter -lavformat -lavutil
*本程序实现了音频PCM采样数据编码为压缩码流（MP3，WMA，AAC等）。
*是最简单的FFmpeg音频编码方面的教程。
*通过学习本例子可以了解FFmpeg的编码流程。
* 技术点: ffmpeg如何读取文件中的音频数据，音频如何解码，如何重采样，音频如何编码，如何封装为指定格式，如何传参数等
* 作者:陈赐常
* Email:99138408@qq.com
* QQ:99138408
* QQ群:60971799
* 日期:2017.4.14
* 参考了ffmpeg.c里面的filter_complex的代码:
* 此文档的方法接近于ffmpeg中对滤镜处理的方法，对学习ffmpeg的滤镜处理流程很有帮助，
* 虽然ffmpeg_overlay_1.c也能实现添加水印，但是比较简单
*/

#ifdef _MSC_VER

#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif

#ifndef __cplusplus
#define inline __inline
#endif

#ifndef snprintf
#define snprintf _snprintf
#endif

#endif

#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"

#ifdef __cplusplus
};
#endif



const char *filter_descr = "overlay=5:5";

#define ENABLE_YUV_FILE 1

AVFormatContext *input_fmt_ctx;
AVCodecContext *input_dec_ctx;

AVFormatContext *overlay_fmt_ctx;
AVCodecContext *overlay_dec_ctx;

int input_video_stream_idx, overlay_video_stream_idx;

AVFilterGraph *filter_graph;

AVFilterInOut *inputs;
AVFilterInOut *outputs;

AVFilterContext *buffersrc_ctx;
AVFilterContext *bufferoverlay_ctx;
AVFilterContext *buffersink_ctx;

int ret;
int got_frame;

int video_eof_reached = 0;
int overlay_eof_reached = 0;

int active_stream_index = -1;

FILE* fp_yuv;

void yuv420p_save(AVFrame *pFrame);
int video_transcode_step(AVFrame* mVideoFrame);
int overlay_transcode_step(AVFrame* mOverlayFrame);
int video_output_eof_packet(const char* tag,
	AVStream* ist, AVFilterContext* ifilter);

static int open_input_file(const char *filename)
{
	int ret;
	AVCodec *dec;

	if ((ret = avformat_open_input(&input_fmt_ctx, filename, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(input_fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(input_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
		return ret;
	}
	input_video_stream_idx = ret;
	input_dec_ctx = input_fmt_ctx->streams[input_video_stream_idx]->codec;

	/* init the video decoder */
	if ((ret = avcodec_open2(input_dec_ctx, dec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
		return ret;
	}

	return 0;
}


static int open_overlay_file(const char *filename)
{
	int ret;
	AVCodec *dec;

	if ((ret = avformat_open_input(&overlay_fmt_ctx, filename, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(overlay_fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(overlay_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
		return ret;
	}
	overlay_video_stream_idx = ret;
	overlay_dec_ctx = overlay_fmt_ctx->streams[overlay_video_stream_idx]->codec;

	/* init the video decoder */
	if ((ret = avcodec_open2(overlay_dec_ctx, dec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
		return ret;
	}

	printf("overlay format = %s\n", overlay_fmt_ctx->iformat->name);

	return 0;
}


static int video_config_input_filter(AVFilterInOut* inputs, AVFilterContext** input_filter_ctx)
{
	char args[512];
	memset(args, 0, sizeof(args));

	AVFilterContext *first_filter = inputs->filter_ctx;
	int pad_idx = inputs->pad_idx;

	AVFilter *filter = avfilter_get_by_name("buffer");
	//	AVRational time_base = input_dec_ctx->time_base;
	AVStream* video_st = input_fmt_ctx->streams[input_video_stream_idx];
	AVRational time_base = video_st->time_base;

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=%d:frame_rate=%d/%d",
		input_dec_ctx->width, input_dec_ctx->height, input_dec_ctx->pix_fmt,
		input_dec_ctx->time_base.num, input_dec_ctx->time_base.den,
		input_dec_ctx->sample_aspect_ratio.num, input_dec_ctx->sample_aspect_ratio.den,
		SWS_BILINEAR + ((video_st->codec->flags&CODEC_FLAG_BITEXACT) ? SWS_BITEXACT : 0),
		video_st->r_frame_rate.num, video_st->r_frame_rate.den);

	printf("input args = %s\n", args);

	ret = avfilter_graph_create_filter(input_filter_ctx, filter, "src_in", args, NULL, filter_graph);

	if (ret < 0) {
		printf("video config input filter fail.\n");
		return -1;
	}
	ret = avfilter_link(*input_filter_ctx, 0, first_filter, pad_idx);
	assert(ret >= 0);

	printf("video_config_input_filter avfilter_link ret = %d\n", ret);

	return ret;

}

static int video_config_overlay_filter(AVFilterInOut* inputs, AVFilterContext** overlay_filter_ctx)
{
	char args[512];
	memset(args, 0, sizeof(args));

	AVFilterContext *first_filter = inputs->filter_ctx;

	int pad_idx = inputs->pad_idx;

	AVFilter *filter = avfilter_get_by_name("buffer");

	//AVRational time_base = overlay_dec_ctx->time_base;
	AVStream* overlay_st = overlay_fmt_ctx->streams[overlay_video_stream_idx];
	AVRational time_base = overlay_st->time_base;

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=%d:frame_rate=%d/%d",
		overlay_dec_ctx->width, overlay_dec_ctx->height, overlay_dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		overlay_dec_ctx->sample_aspect_ratio.num, overlay_dec_ctx->sample_aspect_ratio.den,
		SWS_BILINEAR + ((overlay_st->codec->flags&CODEC_FLAG_BITEXACT) ? SWS_BITEXACT : 0),
		overlay_st->r_frame_rate.num, overlay_st->r_frame_rate.den);

	printf("overlay args = %s\n", args);

	ret = avfilter_graph_create_filter(overlay_filter_ctx, filter, "overlay_in", args, NULL, filter_graph);

	if (ret < 0) {
		printf("video config overlay filter fail.\n");
		return -1;
	}

	ret = avfilter_link(*overlay_filter_ctx, 0, first_filter, pad_idx);
	assert(ret >= 0);

	printf("video_config_overlay_filter ret = %d\n", ret);

	avfilter_inout_free(&inputs);

	return ret;

}

static int video_config_output_filter(AVFilterInOut* outputs, AVFilterContext** out_filter_ctx)
{
	char args[512];

	AVFilterContext *last_filter = outputs->filter_ctx;
	int pad_idx = outputs->pad_idx;
	AVFilter *buffersink = avfilter_get_by_name("buffersink");

	int ret = avfilter_graph_create_filter(out_filter_ctx, buffersink, "video_out", NULL, NULL, filter_graph);
	assert(ret >= 0);

	if (ret < 0)
		return ret;

	ret = avfilter_link(last_filter, pad_idx, *out_filter_ctx, 0);
	assert(ret >= 0);
	if (ret < 0)
		return ret;

	avfilter_inout_free(&outputs);

	return 0;

}

static int init_input_filters()
{
	filter_graph->scale_sws_opts = av_strdup("flags=0x4");
	av_opt_set(filter_graph, "aresample_swr_opts", "", 0);

	ret = avfilter_graph_parse2(filter_graph, filter_descr, &inputs, &outputs);

	assert(inputs && inputs->next && !inputs->next->next);

	ret = video_config_input_filter(inputs, &buffersrc_ctx);

	ret = video_config_overlay_filter(inputs->next, &bufferoverlay_ctx);

	return ret;
}

static int init_output_filters()
{
	return video_config_output_filter(outputs, &buffersink_ctx);
}

int reap_filters() {
/*	AVFilterBufferRef *picref;

	while (1) {
		ret = av_buffersink_get_buffer_ref(buffersink_ctx, &picref, AV_BUFFERSINK_FLAG_NO_REQUEST);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			//printf("reap_filters fail ret = %d\n", ret);
			return 0; // no frame filtered.
		}

		printf("samplesref -------------------\n");

		AVFrame* filtered_frame = avcodec_alloc_frame();
		avcodec_get_frame_defaults(filtered_frame);

		avfilter_copy_buf_props(filtered_frame, picref);

		yuv420p_save(filtered_frame);

		avfilter_unref_bufferp(&picref);
	}
	*/
	AVFrame *filtered_frame = av_frame_alloc();
	if (!filtered_frame)
	{
		return -1;
	}
	while (1)
	{		
		int ret = av_buffersink_get_frame_flags(buffersink_ctx, filtered_frame,
			AV_BUFFERSINK_FLAG_NO_REQUEST);
		if (ret < 0)
		{
			if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
			{
				return ret;
			}
			break;
		}
		yuv420p_save(filtered_frame);
		av_frame_unref(filtered_frame);
	}
	av_frame_free(&filtered_frame);
}

int transcode_from_filter(AVFilterContext** ifilters, int* eof_reached_arr, int* active_stream_indext) {
	int ret = 0;

	ret = avfilter_graph_request_oldest(filter_graph);

	if (ret >= 0) {
		return ret;
	}
	if (ret == AVERROR_EOF) {
		return ret;
	}
	if (ret != AVERROR(EAGAIN)) {
		return ret;
	}

	int nb_requests_max = 0;
	int i;
	for (i = 0; i < 2; i++) {
		int eof_reached = eof_reached_arr[i];
		if (eof_reached) {
			continue;
		}
		AVFilterContext* ifilter = ifilters[i];
		int nb_requests = av_buffersrc_get_nb_failed_requests(ifilter);
		if (nb_requests > nb_requests_max) {
			nb_requests_max = nb_requests;
			*active_stream_indext = i;
		}
	}

	return ret;

}


int main(int argc,char* argv[])
{

#ifdef _MSC_VER
	char* video_file = "H:\\iis.mp4";
	char* overlay_video_file = "H:\\logo.jpg";
#else
	char* video_file = "outFileSrc.mp4";
	char* overlay_video_file = "my_logo.png";
#endif

	AVFrame* mVideoFrame = NULL;
	AVFrame* mOverlayFrame = NULL;

#if ENABLE_YUV_FILE
	const char* yuvFile = "outWater.yuv";
	fp_yuv = fopen(yuvFile, "wb");
	if (!fp_yuv)
	{
		return -1;
	}
#endif

	av_register_all();
	avfilter_register_all();
	avformat_network_init();

	open_input_file(video_file);
	open_overlay_file(overlay_video_file);

	filter_graph = avfilter_graph_alloc();
	if (!filter_graph) {
		printf("filter graph alloc fail.\n");
		return -1;
	}

	init_input_filters();
	init_output_filters();

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		return ret;

	mVideoFrame = av_frame_alloc();
	mOverlayFrame = av_frame_alloc();

	while (1) {
		if (video_eof_reached && overlay_eof_reached) {
			printf("stream EOF.\n");
			break;
		}

		AVFilterContext* ifilters[] = { buffersrc_ctx, bufferoverlay_ctx };
		int eof_reacheds[] = { video_eof_reached, overlay_eof_reached };

		ret = transcode_from_filter(ifilters, eof_reacheds, &active_stream_index);

		if (ret >= 0) {
			ret = reap_filters();
			assert(ret >= 0);
			continue;
		}
		if (ret == AVERROR_EOF) {
			ret = reap_filters();
			assert(ret >= 0);
			continue;
		}

		if (ret == AVERROR(EAGAIN) && active_stream_index < 0) {
			continue;
		}
		assert(active_stream_index >= 0);
		printf("active_stream_index = %d\n", active_stream_index);

		if (active_stream_index == 0) {
			video_transcode_step(mVideoFrame);
			continue;
		}
		overlay_transcode_step(mOverlayFrame);
	}

	if (input_dec_ctx)
		avcodec_close(input_dec_ctx);
	avformat_close_input(&input_fmt_ctx);

	if (overlay_dec_ctx)
		avcodec_close(overlay_dec_ctx);
	avformat_close_input(&overlay_fmt_ctx);

	printf("my_filtering_video3 end -------\n");
	return 0;
}

int video_transcode_step(AVFrame* mVideoFrame) {
	int ret = 0;
	AVPacket pkt;

	ret = av_read_frame(input_fmt_ctx, &pkt);
	if (ret == AVERROR(EAGAIN)) {
		return 0;
	}
	if (ret < 0) {
		video_eof_reached = 1;
		assert(ret == AVERROR_EOF);
		ret = video_output_eof_packet("video_eof", input_fmt_ctx->streams[input_video_stream_idx], buffersrc_ctx);
		assert(ret >= 0);
		return ret;

	}

	if (pkt.stream_index != input_video_stream_idx) {
		//		av_free(&pkt);
		return ret;
	}

	ret = avcodec_decode_video2(input_dec_ctx, mVideoFrame, &got_frame, &pkt);
	if (ret < 0) {
		printf("Error decoding input video\n");
	}

	if (got_frame) {
		int64_t best_effort_timestamp = av_frame_get_best_effort_timestamp(mVideoFrame);
		mVideoFrame->pts = best_effort_timestamp;

		if (av_buffersrc_add_frame_flags(buffersrc_ctx, mVideoFrame, AV_BUFFERSRC_FLAG_PUSH) < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error while feeding the video filtergraph\n");
			return -1;
		}
		reap_filters();
	}

	return 0;

}

int overlay_transcode_step(AVFrame* mOverlayFrame) {
	int ret = 0;
	AVPacket pkt;

	ret = av_read_frame(overlay_fmt_ctx, &pkt);
	if (ret == AVERROR(EAGAIN)) {
		return 0;
	}

	if (ret < 0) {
		overlay_eof_reached = 1;
		ret = video_output_eof_packet("overlay_eof", overlay_fmt_ctx->streams[input_video_stream_idx], bufferoverlay_ctx);
		assert(ret >= 0);
		return ret;
	}

	if (pkt.stream_index != overlay_video_stream_idx) {
		av_free_packet(&pkt);
		return ret;
	}

	ret = avcodec_decode_video2(overlay_dec_ctx, mOverlayFrame, &got_frame, &pkt);
	if (ret < 0) {
		printf("Error decoding overlay video\n");
	}

	if (got_frame) {
		int64_t best_effort_timestamp = av_frame_get_best_effort_timestamp(mOverlayFrame);

		mOverlayFrame->pts = best_effort_timestamp;

		if (av_buffersrc_add_frame_flags(bufferoverlay_ctx, mOverlayFrame, AV_BUFFERSRC_FLAG_PUSH) < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
			return -1;
		}
	}
	return 0;

}

/**
* output EOF packet to filter to flush
*/
int video_output_eof_packet(const char* tag,
	AVStream* ist, AVFilterContext* ifilter)
{
	int ret = 0;

	// alloc frame if NULL
	AVFrame* decoded_frame = av_frame_alloc();

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	int got_frame = 0;
	ret = avcodec_decode_video2(ist->codec, decoded_frame, &got_frame, &pkt);
	// EOF, assert got nothing and ret is 0.
	// TODO: here we still got frame, different to ffmpeg.
	assert(ret >= 0);

	// flush filter
	av_buffersrc_add_frame_flags(ifilter, NULL, AV_BUFFERSRC_FLAG_PUSH);

	printf("[%s] filter -> eof packet.\n", tag);

	return ret;
}


/**
* save yuv420p frame [YUV]
*/
void yuv420p_save(AVFrame *pFrame)
{
	int i = 0;

	int width = pFrame->width, height = pFrame->height;
	int height_half = height / 2, width_half = width / 2;
	int y_wrap = pFrame->linesize[0];
	int u_wrap = pFrame->linesize[1];
	int v_wrap = pFrame->linesize[2];

	unsigned char *y_buf = pFrame->data[0];
	unsigned char *u_buf = pFrame->data[1];
	unsigned char *v_buf = pFrame->data[2];

	//save y  
	for (i = 0; i < height; i++)
		fwrite(y_buf + i * y_wrap, 1, width, fp_yuv);

	//save u  
	for (i = 0; i < height_half; i++)
		fwrite(u_buf + i * u_wrap, 1, width_half, fp_yuv);

	//save v  
	for (i = 0; i < height_half; i++)
		fwrite(v_buf + i * v_wrap, 1, width_half, fp_yuv);

	fflush(fp_yuv);
}