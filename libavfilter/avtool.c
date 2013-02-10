/*
 * avtool
 * copyright (c) 2008 ψ（プサイ）
 *
 * さきゅばす用に拡張されたVhookライブラリから
 * 使われるライブラリです。
 *
 * このファイルは「さきゅばす」の一部であり、
 * このソースコードはGPLライセンスで配布されますです。
 */
#include <stdio.h>
#include "common/framehook_ext.h"
#include "avtool.h"
#include "../libavutil/log.h"

static toolbox Box = {
	.version = TOOLBOX_VERSION,
	.video_length = 0.0f
};

/* こちらはffmpeg側から呼ばれる関数 */

int tool_registerInfo(int64_t duration,int64_t rec_time){
	double const dur = ((double)duration)/AV_TIME_BASE;
	double const rectime = ((double)rec_time)/AV_TIME_BASE;
	Box.video_length = (dur > rectime && rectime > 0) ? rectime : dur;
    av_log(NULL, AV_LOG_DEBUG, "<info for Saccubus registered> duration: %f rectime: %f => len: %f\n", dur, rectime, Box.video_length);
	return 0;
}

const toolbox* tool_getToolBox(){
	return &Box;
}
