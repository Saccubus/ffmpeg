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
#include <limits.h>
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "saccubus_adapter.h"
#include "avdevice.h"

//ダイナミックロード
#if HAVE_DLFCN_H
#include <dlfcn.h>
#else
//dlfcn.hの無いWindows Mingw環境用
#include <windows.h>
#define dlopen(a,b) ((void*)LoadLibrary(a))
#define dlsym(a,b) ((void*)GetProcAddress((HMODULE)(a),(b)))
#define dlclose(a) FreeLibrary((HMODULE)(a));
#define dlerror() "dlerror()"
#define RTLD_NOW 0
#endif

#define SACC_DELIM '#'

#define COLOR_FORMAT (AV_PIX_FMT_RGB32)

#define VIDEO_STREAM (0)
#define AUDIO_STREAM (1)

typedef struct {
	AVClass *klass;///< class for private options
	int eof;
	int videoCount;
/* 動画管理 */
	AVFormatContext *formatContext;
	int videoStreamIndex;
	int audioStreamIndex;
	AVFrame* rawFrame;
	AVFrame* scaledFrame;
	AVFrame* dstFrame;
	struct SwsContext *swsContext;
	int scaledBufferSize;
	uint8_t* scaledBuffer;
	int dstBufferSize;
	uint8_t* dstBuffer;
	int srcWidth, srcHeight;
	int scaledWidth, scaledHeight;
	int dstWidth, dstHeight;
/* fps管理 */
	int minfps;
	int fpsFactor;
	int frameLeft;
	int64_t pktDuration;
	int64_t pktPos;
	int64_t pktDts;
	AVRational dstFrameTime;
	AVRational dstTimebase;
/* 本体に渡すツールボックス */
	SaccToolBox toolbox;
/* 本体の関数ポインタ */
	void* saccDynamic;
	void* saccPriv;
	char* arg;
	int argc;
	char** argv;
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
static int SaccContext_configureAdapter(SaccContext* const self, const char* const filename);
static int SaccContext_loadAdapter(SaccContext* const self, const char* const filename);
static void SaccContext_releaseAdapter(SaccContext* const self);
static int SaccToolBox_loadVideo(SaccToolBox* const box, const char* const filename);

//---------------------------------------------------------------------------------------------------------------------
#define OFFSET(x) offsetof(SaccContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
	{ "sacc", "SaccubusArgs", OFFSET(arg),  AV_OPT_TYPE_STRING, {.str = NULL }, 0,  0, DEC },
	{ "width", "width", OFFSET(scaledWidth), AV_OPT_TYPE_INT, {.dbl = 0}, 0, INT_MAX, DEC},
	{ "height", "height", OFFSET(scaledHeight), AV_OPT_TYPE_INT, {.dbl = 0}, 0, INT_MAX, DEC},
	{ "minfps", "Minimum fps", OFFSET(minfps), AV_OPT_TYPE_INT, {.dbl = 0}, 0, DBL_MAX, DEC},
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
	if(SaccContext_loadAdapter(self, avctx->filename) < 0) return -1;
	if(SaccContext_configureAdapter(self, avctx->filename) < 0) return -1;

	{ /* VIDEOストリームの初期化 */
		AVStream *st;
		const AVStream *orig = self->formatContext->streams[self->videoStreamIndex];
		if (!(st = avformat_new_stream(avctx, NULL))){
			return AVERROR(ENOMEM);
		}
		st->id = VIDEO_STREAM;
		st->codec->codec_type = AVMEDIA_TYPE_VIDEO;

		st->codec->codec_id					= AV_CODEC_ID_RAWVIDEO;
		st->codec->pix_fmt					= COLOR_FORMAT;

		st->r_frame_rate					= orig->r_frame_rate;
		st->start_time						= orig->start_time;
		st       ->time_base				= orig       ->time_base;
		st->codec->time_base				= orig->codec->time_base;
		if(self->minfps > 0){
			self->fpsFactor = ceil((double)self->minfps*st->r_frame_rate.den/st->r_frame_rate.num);
			st       ->time_base.den *= self->fpsFactor;
			st->codec->time_base.den *= self->fpsFactor;
			st->r_frame_rate.num *= self->fpsFactor;
		}else{
			self->fpsFactor = 1;
		}
		{
			AVRational sec = { st->time_base.den, st->time_base.num };
			self->dstFrameTime = av_div_q(sec, st->r_frame_rate);
			self->dstTimebase = st->time_base;
		}
		self->frameLeft = 0;
		self->pktDuration = 0;
		self->pktPos = 0;
		self->pktDts = AV_NOPTS_VALUE;
		av_log(self, AV_LOG_WARNING, "FPS Factor: %d (%f -> %f), min %dfps\n",
				self->fpsFactor,
				(double)self->formatContext->streams[self->videoStreamIndex]->r_frame_rate.num/self->formatContext->streams[self->videoStreamIndex]->r_frame_rate.den,
				(double)st->r_frame_rate.num/st->r_frame_rate.den,
				self->minfps
				);

		st->codec->width					= self->dstWidth;
		st->codec->height					= self->dstHeight;

		st       ->sample_aspect_ratio	= self->formatContext->streams[self->videoStreamIndex]       ->sample_aspect_ratio;
		st->codec->sample_aspect_ratio	= self->formatContext->streams[self->videoStreamIndex]->codec->sample_aspect_ratio;
	}
	
	{ /* AUDIOストリームの初期化 */
		const AVStream *orig = self->formatContext->streams[self->audioStreamIndex];
		AVStream *st;
		if (!(st = avformat_new_stream(avctx, self->formatContext->streams[self->audioStreamIndex]->codec->codec))){
			return AVERROR(ENOMEM);
		}
		st->id				= AUDIO_STREAM;
		st->r_frame_rate	= orig->r_frame_rate;
		st->pts				= orig->pts;
		st->duration		= orig->duration;
		st->duration		= orig->duration;
		st->nb_frames		= orig->nb_frames;
		st->nb_index_entries= orig->nb_index_entries;
		st->start_time		= orig->start_time;
		st->time_base		= orig->time_base;
		st->discard			= orig->discard;
		st->disposition		= orig->disposition;
		avcodec_copy_context(st->codec, orig->codec);
	}
	return 0;
}

static av_cold int SaccContext_delete(AVFormatContext *avctx)
{
	SaccContext* const self = (SaccContext*)avctx->priv_data;
	av_log(self, AV_LOG_WARNING, "Closing...\n");
	SaccContext_closeCodec(self);
	SaccContext_releaseAdapter(self);
	SaccContext_clear(self);
	return 0;
}

static int createVideoPacket(SaccContext* const self, AVPacket *pkt)
{
	const int64_t dstDts = self->pktDts+(self->dstFrameTime.num*(self->fpsFactor-self->frameLeft)/self->dstFrameTime.den);
	self->frameLeft--;

	{ /* dstにさきゅばすを合成 */
		double const pts = dstDts * av_q2d(self->dstTimebase);
		SaccFrame dstFrame;
		{
			dstFrame.vpos = pts;
			dstFrame.data = self->dstFrame->data[self->dstFrame->display_picture_number];
			dstFrame.linesize = self->dstFrame->linesize[self->dstFrame->display_picture_number];
			dstFrame.w = self->dstWidth;
			dstFrame.h = self->dstHeight;
		}
		SaccFrame videoFrame;
		{
			videoFrame.vpos = pts;
			videoFrame.data = self->scaledFrame->data[self->scaledFrame->display_picture_number];
			videoFrame.linesize = self->scaledFrame->linesize[self->scaledFrame->display_picture_number];
			videoFrame.w = self->scaledWidth;
			videoFrame.h = self->scaledHeight;
		}
		self->saccProcess(self->saccPriv, &self->toolbox, &dstFrame, &videoFrame);
	}

	{ // dstの構築
		AVPicture dstPict;
		memcpy(dstPict.data,     self->dstFrame->data,     AV_NUM_DATA_POINTERS*sizeof(self->dstFrame->data[0]));
		memcpy(dstPict.linesize, self->dstFrame->linesize, AV_NUM_DATA_POINTERS*sizeof(self->dstFrame->linesize[0]));
		/* パケットの構築 */
		const int pret = av_new_packet(pkt, self->dstBufferSize);
		if (pret < 0) {
			return pret;
		}
		avpicture_layout(&dstPict, COLOR_FORMAT, self->dstWidth, self->dstHeight, pkt->data, self->dstBufferSize);
	}
	pkt->stream_index = VIDEO_STREAM;
	pkt->duration = self->pktDuration;
	pkt->dts = dstDts;
	pkt->pts = dstDts;
	pkt->pos = self->pktPos;
	pkt->size = self->dstBufferSize;
	return 0;
}

static int SaccContext_readPacket(AVFormatContext *avctx, AVPacket *pkt)
{
	SaccContext* const self = (SaccContext*)avctx->priv_data;
	if(self->eof){
		return AVERROR_EOF;
	}
	if(self->frameLeft > 0){
		return createVideoPacket(self, pkt);
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
					self->scaledFrame->data,
					self->scaledFrame->linesize);
				self->pktDuration = packet.duration * self->fpsFactor;
				self->pktPos = packet.pos;
				// FIXME:DTSとPTSの区別をつける
				// DTS!=DPSなフレームが来たら貯めこんでおいて、DTS==DPSなフレームが来たら一気に戻せばOK?
				// http://htffmpegx.seesaa.net/article/15410871.html
				self->pktDts = packet.dts != AV_NOPTS_VALUE ? (packet.dts * self->fpsFactor) : 0;
				self->frameLeft = self->fpsFactor;
				av_free_packet(&packet);
				ret = createVideoPacket(self, pkt);
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
	self->scaledFrame = NULL;
	self->dstFrame = NULL;
	self->swsContext = NULL;
	self->scaledBufferSize = -1;
	self->scaledBuffer = NULL;
	self->dstBufferSize = -1;
	self->dstBuffer = NULL;
	self->srcWidth=-1;
	self->srcHeight=-1;
	self->dstWidth=-1;
	self->dstHeight=-1;
	
	self->fpsFactor = 1;
	self->frameLeft = 0;
	self->pktDuration = 0;
	self->pktPos = 0;
	self->pktDts = AV_NOPTS_VALUE;
	self->dstFrameTime.den = self->dstFrameTime.num = 0;

	self->toolbox.ptr = self;
	self->toolbox.version = TOOLBOX_VERSION;
	self->toolbox.loadVideo = SaccToolBox_loadVideo;
	self->toolbox.seek = NULL;//FIXME
	self->toolbox.currentVideo.width = -1;
	self->toolbox.currentVideo.height = -1;
	self->toolbox.currentVideo.length = -1;

	self->saccDynamic = NULL;
	self->saccPriv = NULL;
	//self->arg = NULL;
	self->argc = 0;
	self->argv = NULL;
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
	if(self->scaledBuffer != NULL){
		av_free(self->scaledBuffer);
	}
	self->scaledBuffer = NULL;
	if(self->scaledFrame != NULL){
		av_free(self->scaledFrame);
	}
	self->scaledFrame = NULL;
	if(self->dstBuffer != NULL){
		av_free(self->dstBuffer);
	}
	self->dstBuffer = NULL;
	if(self->dstFrame != NULL){
		av_free(self->dstFrame);
	}
	self->dstFrame = NULL;
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

static int SaccContext_loadAdapter(SaccContext* const self, const char* const filename)
{
	self->saccDynamic = dlopen(filename, RTLD_NOW);
	if(!self->saccDynamic){
		av_log(self, AV_LOG_ERROR, "Failed to load saccubus adapter: %s\nBecause: %s\n", filename, dlerror());
		return -1;
	}
	self->saccConfigure = dlsym(self->saccDynamic, "SaccConfigure");
	self->saccProcess = dlsym(self->saccDynamic, "SaccProcess");
	self->saccMeasure = dlsym(self->saccDynamic, "SaccMeasure");
	self->saccRelease = dlsym(self->saccDynamic, "SaccRelease");
	
	if(!self->saccConfigure)
	{
		av_log(self, AV_LOG_ERROR, "Failed to resolve function: SaccConfigure\n");
		return -1;
	}
	if(!self->saccProcess)
	{
		av_log(self, AV_LOG_ERROR, "Failed to resolve function: SaccProcess\n");
		return -1;
	}
	if(!self->saccMeasure)
	{
		av_log(self, AV_LOG_ERROR, "Failed to resolve function: SaccMeasure\n");
		return -1;
	}
	if(!self->saccRelease)
	{
		av_log(self, AV_LOG_ERROR, "Failed to resolve function: SaccRelease\n");
		return -1;
	}
	return 0;
}

static void SaccContext_releaseAdapter(SaccContext* const self)
{
	for(int i=0;i<self->argc;++i){
		av_free(self->argv[i]);
	}
	av_free(self->argv);
	self->saccRelease(self->saccPriv, &self->toolbox);
	dlclose(self->saccDynamic);
}

static char* copyString(const char* string, int from, int to)
{
	if(to < 0){
		to = strlen(string);
	}
	const int size = to - from;
	char* ret = av_malloc(size+1);
	memcpy(ret, &string[from], size);
	ret[size]='\0';
	return ret;
}

static int SaccContext_configureAdapter(SaccContext* const self, const char* const filename)
{
	if(!self->arg){
		av_log(self, AV_LOG_ERROR, "You need to set \"-sacc\" option!!\n");
		return -1;
	}
	self->argc = 1;
	self->argv = (char**)av_malloc(sizeof(char*));
	self->argv[0] = copyString(filename, 0, -1);

	const int arglen = strlen(self->arg);
	int last = 0;

	for(int i=0;i<arglen;++i){
		if(self->arg[i] == SACC_DELIM){
			++self->argc;
			self->argv=(char**)av_realloc(self->argv, self->argc * sizeof(char*));
			self->argv[self->argc-1] = copyString(self->arg, last, i);
			last = i+1;
		}
	}
	++self->argc;
	self->argv=(char**)av_realloc(self->argv, self->argc * sizeof(char*));
	self->argv[self->argc-1] = copyString(self->arg, last, -1);
	for(int i=0;i<self->argc;++i){
		av_log(self, AV_LOG_WARNING, "arg[% 2d]=> %s\n", i, self->argv[i]);
	}

	const int ret = self->saccConfigure(&self->saccPriv, &self->toolbox, self->argc, self->argv);
	if(ret < 0){
		av_log(self, AV_LOG_ERROR, "Failed to configure adapter.\n");
	}
	return ret;
}

static int SaccToolBox_loadVideo(SaccToolBox* const box, const char* const filename)
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
	
	self->scaledWidth = self->scaledWidth <= 0 ? self->srcWidth : self->scaledWidth;
	self->scaledHeight = self->scaledHeight <= 0 ? self->srcHeight : self->scaledHeight;
	if((self->srcWidth*self->scaledHeight/self->srcHeight/self->scaledWidth) < 1){ //scaledの方が横長
		self->scaledWidth = self->srcWidth * self->scaledHeight / self->srcHeight;
	}else{
		self->scaledHeight = self->srcHeight * self->scaledWidth / self->srcWidth;
	}

	self->toolbox.currentVideo.width = self->scaledWidth;
	self->toolbox.currentVideo.height = self->scaledHeight;
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

	av_log(self, AV_LOG_WARNING, "source size: %dx%d length: %fsec\n", self->srcWidth, self->srcHeight, self->toolbox.currentVideo.length);


	if(self->videoCount <= 0){
		self->saccMeasure(self->saccPriv, &self->toolbox, self->scaledWidth, self->scaledHeight, &self->dstWidth, &self->dstHeight);
		av_log(self, AV_LOG_WARNING, "size detected: %dx%d -> %dx%d -> %dx%d\n",
				self->srcWidth, self->srcHeight,
				self->scaledWidth, self->scaledHeight,
				self->dstWidth, self->dstHeight
				);
		//XXX: 各種フォーマットで許容される辺のサイズは違うけど、
		// そういった制約を統一的に調べる方法はない。でも大体４の倍数なら許されるのでそれで誤魔化す。ひどい。
		int const paddedDstW = 
				(self->dstWidth & 3) >= 2 ? (self->dstWidth & ~3)+4 : (self->dstWidth & ~3);
		int const paddedDstH = 
				(self->dstHeight & 3) >= 2 ? (self->dstHeight & ~3)+4 : (self->dstHeight & ~3);
		if ( self->dstWidth != paddedDstW || self->dstHeight != paddedDstH ) {
			av_log(self, AV_LOG_WARNING, "XXX: dimension should be multiples of 4: %dx%d -> %dx%d\n",
					self->dstWidth, self->dstHeight,
					paddedDstW, paddedDstH
				  );
			self->dstWidth = paddedDstW;
			self->dstHeight = paddedDstH;
		}
	}
	self->videoCount++;

	self->rawFrame = av_frame_alloc();
	self->scaledFrame = av_frame_alloc();
	self->dstFrame = av_frame_alloc();

	self->swsContext = sws_getContext(self->srcWidth, self->srcHeight, self->formatContext->streams[self->videoStreamIndex]->codec->pix_fmt, self->scaledWidth, self->scaledHeight, COLOR_FORMAT, SWS_BICUBIC, 0, 0, 0);

	self->scaledBufferSize = avpicture_get_size(COLOR_FORMAT, self->scaledWidth, self->scaledHeight)*sizeof(uint8_t);
	self->scaledBuffer = (uint8_t*)av_malloc(self->scaledBufferSize);
	avpicture_fill((AVPicture*)self->scaledFrame, self->scaledBuffer, COLOR_FORMAT, self->scaledWidth, self->scaledHeight);

	self->dstBufferSize = avpicture_get_size(COLOR_FORMAT, self->dstWidth, self->dstHeight)*sizeof(uint8_t);
	self->dstBuffer = (uint8_t*)av_malloc(self->dstBufferSize);
	avpicture_fill((AVPicture*)self->dstFrame, self->dstBuffer, COLOR_FORMAT, self->dstWidth, self->dstHeight);

	return 0;
}


