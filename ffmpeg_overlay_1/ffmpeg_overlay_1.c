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
* 日期:2017.4.13
* 参考文档:ffmpeg 3.2.4源码中doc/examples/filtering_video.c文件
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

#ifdef __cplusplus
};
#endif

#ifdef _MSC_VER
/*:\都为滤镜中的关键字需要多个\隔开 */
const char *filter_descr = "movie=H\\\\:\\\\logo.jpg[overlog];[in][overlog]overlay=10:main_h-overlay_h-10";
#else
const char *filter_descr = "movie=logo.jpg[overlog];[in][overlog]overlay=10:main_h-overlay_h-10";
#endif
/* other way:
scale=78:24 [scl]; [scl] transpose=cclock // assumes "[in]" and "[out]" to be input output pads respectively
*/

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;
static int64_t last_pts = AV_NOPTS_VALUE;
AVCodecContext* enc_ctx;
AVCodec* enc_codec;
FILE* out_file = NULL;


static int open_output_file()
{
	int ret = 0;
	AVDictionary *options = NULL;
	AVCodec* enc_codec = avcodec_find_encoder_by_name("libx264");
	enc_ctx = avcodec_alloc_context3(enc_codec);
	if (!enc_ctx)
		return AVERROR(ENOMEM);

	enc_ctx->time_base.num = dec_ctx->time_base.num;
	enc_ctx->time_base.den = dec_ctx->time_base.den;
	enc_ctx->pix_fmt = dec_ctx->pix_fmt;
	enc_ctx->width = dec_ctx->width;
	enc_ctx->height = dec_ctx->height;
	enc_ctx->has_b_frames = dec_ctx->has_b_frames;
	enc_ctx->bit_rate = 1500 * 1024;
	enc_ctx->gop_size = 250;
	
	
	av_dict_set(&options, "profile", "main",0);
	av_dict_set(&options, "tune", "zerolatency",0);
	av_dict_set(&options, "preset", "medium", 0);
	ret = avcodec_open2(enc_ctx, enc_codec, &options);
	if (ret){
		av_log(NULL, AV_LOG_ERROR, "open2 error\n");
		av_dict_free(&options);
		return ret;
	}
	av_dict_free(&options);
	out_file = fopen("test.h264", "wb");
	if (!out_file)
		return -1;
	return ret;
}
static int open_input_file(const char *filename)
{
	int ret;
	AVCodec *dec;

	if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
		return ret;
	}
	video_stream_index = ret;
	dec_ctx = fmt_ctx->streams[video_stream_index]->codec;
	av_opt_set_int(dec_ctx, "refcounted_frames", 1, 0);

	/* init the video decoder */
	if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
		return ret;
	}

	return 0;
}

static int init_filters(const char *filters_descr)
{
	char args[512];
	int ret = 0;
	AVFilter *buffersrc = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* buffer video source: the decoded frames from the decoder will be inserted here. */
	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
		args, NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto end;
	}

	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
		NULL, NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
		goto end;
	}
	
	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
		AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
		goto end;
	}

	/*
	* Set the endpoints for the filter graph. The filter_graph will
	* be linked to the graph described by filters_descr.
	*/

	/*
	* The buffer source output must be connected to the input pad of
	* the first filter described by filters_descr; since the first
	* filter input label is not specified, it is set to "in" by
	* default.
	*/
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;

	/*
	* The buffer sink input must be connected to the output pad of
	* the last filter described by filters_descr; since the last
	* filter output label is not specified, it is set to "out" by
	* default.
	*/
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;

	if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
		&inputs, &outputs, NULL)) < 0)
		goto end;

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		goto end;

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	AVPacket packet;
	AVFrame *frame = av_frame_alloc();
	AVFrame *filt_frame = av_frame_alloc();

#ifdef _MSC_VER
	const char* input_file = "H:\\main.ts";
#else
	const char* input_file = "../iis.mp4";
#endif

	if (!frame || !filt_frame) {
		perror("Could not allocate frame");
		exit(1);
	}
	av_register_all();
	avfilter_register_all();

	if ((ret = open_input_file(input_file)) < 0)
		goto end;
	if ((ret = init_filters(filter_descr)) < 0)
		goto end;

	if ((ret = open_output_file()) < 0)
		goto end;
	
	/* read all packets */
	while (1) {
		if ((ret = av_read_frame(fmt_ctx, &packet)) < 0)
			break;

		if (packet.stream_index == video_stream_index) {
			ret = avcodec_send_packet(dec_ctx, &packet);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Error decoding video\n");
				break;
			}
			ret = avcodec_receive_frame(dec_ctx, frame);
			if (ret == AVERROR(EAGAIN))
				continue;
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Error decoding video\n");
				break;
			}

			frame->pts = av_frame_get_best_effort_timestamp(frame);

			/* push the decoded frame into the filtergraph */
			if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
				av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
				break;
			}

			/* pull filtered frames from the filtergraph */
			while (1) {
				AVPacket pkt;
				av_init_packet(&pkt);
				ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0)
					goto end;
				//  display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
				ret = avcodec_send_frame(enc_ctx, filt_frame);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "Error send_frame video\n");
					av_packet_unref(&pkt);
					av_frame_unref(filt_frame);
					break;
				}
				ret = avcodec_receive_packet(enc_ctx, &pkt);
				if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR, "Error receive_packet video\n");
					av_packet_unref(&pkt);
					av_frame_unref(filt_frame);
					break;
				}
				fwrite(pkt.data, 1, pkt.size, out_file);
				av_packet_unref(&pkt);
				av_frame_unref(filt_frame);
			}
			av_frame_unref(frame);	
		}
		av_packet_unref(&packet);
	}

	//should flush ignore here
end:
	avfilter_graph_free(&filter_graph);
	avcodec_close(dec_ctx);
	avformat_close_input(&fmt_ctx);
	av_frame_free(&frame);
	av_frame_free(&filt_frame);

	avcodec_close(enc_ctx);
	if (out_file)
	{
		fclose(out_file);
		out_file = NULL;
	}
	if (ret < 0 && ret != AVERROR_EOF) {
		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
		exit(1);
	}

	exit(0);
}
