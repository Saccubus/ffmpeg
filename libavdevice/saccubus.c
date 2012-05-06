/**
 * Saccubus
 * Copyright (C) 2012 psi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* #define DEBUG */

#include <float.h>
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "saccubus_adapter.h"
#include "avdevice.h"

#define VIDEO_STREAM 0
#define AUDIO_STREAM 1

typedef struct {
	AVClass *klass;///< class for private options
	int eof;
	int videoCount;
/* 動画管理 */
	AVFormatContext *formatContext;
	int videoStreamIndex;
	int audioStreamIndex;
	AVFrame* rawFrame;
	AVFrame* rgbFrame;
	struct SwsContext *swsContext;
	int bufferSize;
	uint8_t* buffer;
	int srcWidth, srcHeight;
	int dstWidth, dstHeight;
/* 本体に渡すツールボックス */
	SaccToolBox toolbox;
/* 本体の関数ポインタ */
	void* sacc;
	char* arg;
	SaccConfigureFnPtr saccConfigure;
	SaccMeasureFnPtr saccMeasure;
	SaccProcessFnPtr saccProcess;
	SaccReleaseFnPtr saccRelease;
} SaccContext;

static av_cold int SaccContext_init(AVFormatContext *avctx);
static av_cold int SaccContext_delete(AVFormatContext *avctx);
static int SaccContext_readPacket(AVFormatContext *avctx, AVPacket *pkt);

static void SaccContext_clear(SaccContext* const self);
static void SaccContext_closeCodec(SaccContext* const self);
static int SaccToolBox_loadVideo(SaccToolBox* box, const char* filename);

//---------------------------------------------------------------------------------------------------------------------
#define OFFSET(x) offsetof(SaccContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
	{ "sacc", "SaccubusArgs", OFFSET(arg),  AV_OPT_TYPE_STRING, {.str = NULL }, 0,  0, DEC },
	{ NULL },
};

static const AVClass avklass = {
	.class_name = "saccubus indev",
	.item_name  = av_default_item_name,
	.option	 = options,
	.version	= LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_saccubus_demuxer = {
	.name		   = "saccubus",
	.long_name	  = NULL_IF_CONFIG_SMALL("Saccubus virtual input device"),
	.priv_data_size = sizeof(SaccContext),
	.read_header	= SaccContext_init,
	.read_packet	= SaccContext_readPacket,
	.read_close	 = SaccContext_delete,
	.flags		  = AVFMT_NOFILE,
	.priv_class	 = &avklass,
};
//---------------------------------------------------------------------------------------------------------------------
static av_cold int SaccContext_init(AVFormatContext *avctx)
{
	SaccContext* const self = (SaccContext*)avctx->priv_data;
	av_log(self, AV_LOG_WARNING, "Initializing...\n");
	SaccContext_clear(self);
	
	SaccToolBox_loadVideo(&self->toolbox, "test.flv");

	{ /* VIDEOストリームの初期化 */
		AVStream *st;
		if (!(st = avformat_new_stream(avctx, NULL))){
			return AVERROR(ENOMEM);
		}
		st->id = VIDEO_STREAM;
		st->codec->codec_type = AVMEDIA_TYPE_VIDEO;

		st->codec->codec_id					= CODEC_ID_RAWVIDEO;
		st->codec->pix_fmt					= PIX_FMT_RGB24;

		st->r_frame_rate					= self->formatContext->streams[self->videoStreamIndex]->r_frame_rate;
		st->start_time						= self->formatContext->streams[self->videoStreamIndex]->start_time;
		st       ->time_base				= self->formatContext->streams[self->videoStreamIndex]       ->time_base;
		st->codec->time_base				= self->formatContext->streams[self->videoStreamIndex]->codec->time_base;

		st->codec->width					= self->dstWidth;
		st->codec->height					= self->dstHeight;

		st       ->sample_aspect_ratio	= self->formatContext->streams[self->videoStreamIndex]       ->sample_aspect_ratio;
		st->codec->sample_aspect_ratio	= self->formatContext->streams[self->videoStreamIndex]->codec->sample_aspect_ratio;
	}
	
	{ /* AUDIOストリームの初期化 */
		AVStream *st;
		if (!(st = avformat_new_stream(avctx, NULL))){
			return AVERROR(ENOMEM);
		}
		st->id							= AUDIO_STREAM;
		st->codec->codec_type		= AVMEDIA_TYPE_AUDIO;

		st->codec->codec_id			= self->formatContext->streams[self->audioStreamIndex]->codec->codec_id;

		st->r_frame_rate			= self->formatContext->streams[self->audioStreamIndex]->r_frame_rate;
		st->start_time				= self->formatContext->streams[self->audioStreamIndex]->start_time;
		st       ->time_base		= self->formatContext->streams[self->audioStreamIndex]       ->time_base;
		st->codec->time_base		= self->formatContext->streams[self->audioStreamIndex]->codec->time_base;

		st->codec->channels			= self->formatContext->streams[self->audioStreamIndex]->codec->channels;
		st->codec->sample_fmt		= self->formatContext->streams[self->audioStreamIndex]->codec->sample_fmt;
		st->codec->sample_rate		= self->formatContext->streams[self->audioStreamIndex]->codec->sample_rate;
		st->codec->channel_layout	= self->formatContext->streams[self->audioStreamIndex]->codec->channel_layout;
	}
	return 0;
}

static av_cold int SaccContext_delete(AVFormatContext *avctx)
{
	SaccContext* const self = (SaccContext*)avctx->priv_data;
	av_log(self, AV_LOG_WARNING, "Closing...\n");
	SaccContext_closeCodec(self);
	SaccContext_clear(self);
	return 0;
}

static int SaccContext_readPacket(AVFormatContext *avctx, AVPacket *pkt)
{
	SaccContext* const self = (SaccContext*)avctx->priv_data;
	if(self->eof){
		return AVERROR_EOF;
	}
	AVPacket packet;
	int ret = 0;
	while((ret = av_read_frame(self->formatContext, &packet)) >= 0){
		if(packet.stream_index == self->videoStreamIndex){
			int gotPicture = 0;
			avcodec_decode_video2(self->formatContext->streams[self->videoStreamIndex]->codec, self->rawFrame, &gotPicture, &packet);
			if(gotPicture){
				sws_scale(
					self->swsContext,
					(const uint8_t * const*)self->rawFrame->data,
					self->rawFrame->linesize, 0, self->srcHeight,
					self->rgbFrame->data,
					self->rgbFrame->linesize);
				const double pts = ( (packet.dts != (int64_t)AV_NOPTS_VALUE) ? packet.dts : 0 ) * av_q2d(self->formatContext->streams[self->videoStreamIndex]->time_base);
				
				//av_log(TAG, AV_LOG_WARNING, "Time:%f\n", pts);	
				//*vpos = (float)pts;
				//*data = self->rgbFrame->data[self->rgbFrame->display_picture_number];
				//*stride = self->rgbFrame->linesize[this->rgbFrame->display_picture_number];

				{
					AVPicture pict;
					int pret = 0;
					if ((pret = av_new_packet(pkt, self->bufferSize)) < 0) {
						return pret;
					}
					memcpy(pict.data,     self->rgbFrame->data,     4*sizeof(self->rgbFrame->data[0]));
					memcpy(pict.linesize, self->rgbFrame->linesize, 4*sizeof(self->rgbFrame->linesize[0]));
					avpicture_layout(&pict, PIX_FMT_RGB24, self->dstWidth, self->dstHeight, pkt->data, self->bufferSize);
				}
				pkt->stream_index = VIDEO_STREAM;
				pkt->duration = packet.duration;
				pkt->pts = packet.pts;
				pkt->pos = packet.pos;
				pkt->size = self->bufferSize;
				av_free_packet(&packet);
				break;
			}
		} else if(packet.stream_index == self->audioStreamIndex){
			*pkt = packet;
			pkt->stream_index = AUDIO_STREAM;
			break;
		} else {
			av_free_packet(&packet);
		}
	}
	if(ret == AVERROR_EOF)
	{
		av_log(self, AV_LOG_WARNING, "video ended.\n");
		self->eof = 1;
	}
	return ret;
}

//---------------------------------------------------------------------------------------------------------------------

static void SaccContext_clear(SaccContext* const self)
{
	self->eof = 0;
	self->videoCount = 0;

	self->formatContext = NULL;
	self->videoStreamIndex = -1;
	self->audioStreamIndex = -1;
	self->rawFrame = NULL;
	self->rgbFrame = NULL;
	self->swsContext = NULL;
	self->bufferSize = -1;
	self->buffer = NULL;
	self->srcWidth=-1;
	self->srcHeight=-1;
	self->dstWidth=-1;
	self->dstHeight=-1;
	
	self->toolbox.ptr = self;
	self->toolbox.version = TOOLBOX_VERSION;
	self->toolbox.loadVideo = SaccToolBox_loadVideo;
	self->toolbox.seek = NULL;//FIXME
	self->toolbox.currentVideo.width = -1;
	self->toolbox.currentVideo.height = -1;
	self->toolbox.currentVideo.length = -1;

	self->sacc = NULL;
	self->saccConfigure = NULL;
	self->saccMeasure = NULL;
	self->saccProcess = NULL;
	self->saccRelease = NULL;
}

static void SaccContext_closeCodec(SaccContext* const self)
{
	/**
	 * 現在開いているコーデックというかファイルがあれば、それをクローズする。
	 * 開いているかどうかの判断に、ポインタが0であるか否かを用いているので、最初の初期化には使えない。
	 */
	if(self->buffer != NULL){
		av_free(self->buffer);
	}
	self->buffer = NULL;
	if(self->rgbFrame != NULL){
		av_free(self->rgbFrame);
	}
	self->rgbFrame = NULL;
	if(self->rawFrame != NULL){
		av_free(self->rawFrame);
	}
	self->rawFrame = NULL;
	if(self->swsContext != NULL){
		sws_freeContext(self->swsContext);
	}
	self->swsContext = NULL;
	if(self->formatContext != NULL)
	{
		avformat_free_context(self->formatContext);
	}
	self->videoStreamIndex=-1;
	self->audioStreamIndex=-1;
	self->formatContext=NULL;
}

static int SaccToolBox_loadVideo(SaccToolBox* box, const char* filename)
{
	SaccContext* const self = (SaccContext*)box->ptr;

	SaccContext_closeCodec(self);

	if(avformat_open_input(&self->formatContext, filename, 0, 0) != 0){
		av_log(self, AV_LOG_ERROR, "Failed to read file: %s\n", filename);
		return -1;
	}
	if(avformat_find_stream_info(self->formatContext, NULL)<0){
		av_log(self, AV_LOG_ERROR, "Failed to read stream info: %s\n", filename);
		return -1;
	}
	self->videoStreamIndex = av_find_best_stream(self->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
	self->audioStreamIndex = av_find_best_stream(self->formatContext, AVMEDIA_TYPE_AUDIO, -1, self->audioStreamIndex, 0, 0);

	AVCodec* const videoCodec = self->videoStreamIndex >= 0 ? avcodec_find_decoder(self->formatContext->streams[self->videoStreamIndex]->codec->codec_id) : 0;
	AVCodec* const audioCodec = self->audioStreamIndex >= 0 ? avcodec_find_decoder(self->formatContext->streams[self->audioStreamIndex]->codec->codec_id) : 0;

	if(avcodec_open2(self->formatContext->streams[self->videoStreamIndex]->codec, videoCodec, NULL) < 0){
		av_log(self, AV_LOG_ERROR, "Failed to open video codec: %s\n", filename);
		return -1;
	}
	if(avcodec_open2(self->formatContext->streams[self->audioStreamIndex]->codec, audioCodec, NULL) < 0){
		av_log(self, AV_LOG_ERROR, "Failed to open audio codec: %s\n", filename);
		return -1;
	}

	self->eof = 0;
	self->srcWidth = self->formatContext->streams[self->videoStreamIndex]->codec->width;
	self->srcHeight = self->formatContext->streams[self->videoStreamIndex]->codec->height;
	
	self->toolbox.currentVideo.width = self->srcWidth;
	self->toolbox.currentVideo.height = self->srcHeight;
	if(self->formatContext->streams[self->videoStreamIndex]->duration > 0){
		self->toolbox.currentVideo.length = 
		self->formatContext->streams[self->videoStreamIndex]->duration *
		av_q2d(self->formatContext->streams[self->videoStreamIndex]->time_base);
	}else if(self->formatContext->streams[self->audioStreamIndex]->duration > 0){
		self->toolbox.currentVideo.length = 
		self->formatContext->streams[self->audioStreamIndex]->duration *
		av_q2d(self->formatContext->streams[self->audioStreamIndex]->time_base);
	}else{
		self->toolbox.currentVideo.length = self->formatContext->duration * av_q2d(AV_TIME_BASE_Q);
	}
	av_log(self, AV_LOG_WARNING, "size: %dx%d length: %fsec\n", self->srcWidth, self->srcHeight, self->toolbox.currentVideo.length);


	if(self->videoCount <= 0){
		//FIXME: サイズを聞かなきゃ！
		//self->saccMeasure(self->sacc, &self->toolbox, self->srcWidth, self->srcHeight, &self->dstWidth, &self->dstHeight);
		self->dstWidth = self->srcWidth;
		self->dstHeight = self->srcHeight;
	}
	self->videoCount++;

	self->rawFrame = avcodec_alloc_frame();
	self->rgbFrame = avcodec_alloc_frame();
	self->swsContext = sws_getContext(self->srcWidth, self->srcHeight, self->formatContext->streams[self->videoStreamIndex]->codec->pix_fmt, self->dstWidth, self->dstHeight, PIX_FMT_RGB24, SWS_BICUBIC, 0, 0, 0);
	self->bufferSize = avpicture_get_size(PIX_FMT_RGB24, self->srcWidth, self->srcHeight)*sizeof(uint8_t);
	self->buffer = (uint8_t*)av_malloc(self->bufferSize);
	avpicture_fill((AVPicture*)self->rgbFrame, self->buffer, PIX_FMT_RGB24, self->srcWidth, self->srcHeight);
	
	return 0;
}


