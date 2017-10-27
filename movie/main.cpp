#pragma comment (lib, "libavutil.a")
#pragma comment (lib, "libavformat.a")
#pragma comment (lib, "libavcodec.a")
#pragma comment (lib, "libswresample.a")
#pragma comment (lib, "libswscale.a")
#pragma comment (lib, "mp3lame.lib")

#include <Windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

extern "C"
{
	#ifndef INT64_C
	#define INT64_C(c) (c ## LL)
	#define UINT64_C(c) (c ## ULL)
	#endif
	#ifndef __STDC_CONSTANT_MACROS
	#  define __STDC_CONSTANT_MACROS
	#endif
	#include <libavutil/opt.h>
	#include <libavutil/mathematics.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libswresample/swresample.h>
}

#define GMEXPORT extern "C" __declspec (dllexport)

#define INPUT_PIXEL_FORMAT					AV_PIX_FMT_RGBA
#define STREAM_VIDEO_PIXEL_FORMAT	    	AV_PIX_FMT_YUV420P

#define RAW_AUDIO_FRAME_SIZE		    	1152
#define STREAM_AUDIO_BIT_RATE				320000
#define STREAM_AUDIO_SAMPLE_RATE			44100
#define STREAM_AUDIO_FRAME_SIZE				1152
#define STREAM_AUDIO_SAMPLE_FORMAT_GM		AV_SAMPLE_FMT_S16
#define STREAM_AUDIO_SAMPLE_FORMAT_MOVIE	AV_SAMPLE_FMT_FLTP
#define STREAM_AUDIO_CHANNEL_LAYOUT			AV_CH_LAYOUT_STEREO
#define STREAM_AUDIO_CHANNELS				2

using namespace std;

// A file of raw data
typedef struct File
{
	vector<AVFrame*> frames;
} File;

// A sound
typedef struct Sound
{
	File* file;
	uint64_t play, start, end;
	double volume;
} Sound;

// File settings
int videoWidth, videoHeight, videoBitRate, videoFrameRate;
bool audioEnabled;

// Media file output
AVFormatContext *outContext;

// Video
AVStream *videoStream;
AVCodec *videoCodec;
AVCodecContext *videoCodecContext;
AVRational videoTimeBase;
uint64_t videoFrameNum;
SwsContext *videoSwsContext;

// Audio
AVStream *audioStream;
AVCodec *audioCodec;
AVCodecContext *audioCodecContext;
AVRational audioTimeBase;
uint64_t audioFrameNum;

// Files
vector<File*> files;
vector<Sound*> sounds;

// Converts to a wide string
wstring towstr(const string str)
{
	wstring buffer;
	buffer.resize(MultiByteToWideChar(CP_UTF8, 0, &str[0], -1, 0, 0));
	MultiByteToWideChar(CP_UTF8, 0, &str[0], -1, &buffer[0], buffer.size());
	return &buffer[0];
}

// DLL main function
BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
	return TRUE;
}

// Create an AVRational
AVRational rat(int num, int den)
{
	AVRational r;
	r.num = num;
	r.den = den;
	return r;
}

// Initialize codecs
GMEXPORT double movie_init()
{
	// Initialize libavcodec, and register all codecs and formats.
	av_register_all();
	avcodec_register_all();

	return 0;
}

// Set movie properties
GMEXPORT double movie_set(double width, double height, double bitRate, double frameRate, double audio)
{
	videoWidth = width;
	videoHeight = height;
	videoBitRate = bitRate;
	videoFrameRate = frameRate;
	audioEnabled = audio;

	return 0;
}

// Starts rendering the movie
GMEXPORT double movie_start(const char* outFile, const char* outFormat)
{
	// Find format
	AVOutputFormat *fmt = av_guess_format(outFormat, NULL, NULL);
	if (!outFormat)
		return -1;

	// Allocate the output media context
	avformat_alloc_output_context2(&outContext, fmt, NULL, NULL);
	if (!outContext)
		return -2;

	// Find video encoder
	videoCodec = avcodec_find_encoder(outContext->oformat->video_codec);
	if (!videoCodec)
		return -3;

	// Start video stream
	videoStream = avformat_new_stream(outContext, videoCodec);
	if (!videoStream)
		return -4;
	videoCodecContext = videoStream->codec;
	videoStream->id = 0;

	// Setup
	videoCodecContext->codec_id = outContext->oformat->video_codec;
	videoCodecContext->bit_rate = videoBitRate;
	videoCodecContext->width = videoWidth;
	videoCodecContext->height = videoHeight;
	videoCodecContext->time_base = rat(1, videoFrameRate);
	videoCodecContext->pix_fmt = STREAM_VIDEO_PIXEL_FORMAT;
	videoCodecContext->gop_size = 12; // Emit one intra frame every twelve frames at most
	videoCodecContext->mb_decision = 2;

	if (outContext->oformat->flags & AVFMT_GLOBALHEADER)
		videoCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;

	// Open the codec
	if (avcodec_open2(videoCodecContext, videoCodec, NULL) < 0)
		return -5;

	// Scaling context
	videoSwsContext = sws_getContext(videoWidth, videoHeight, INPUT_PIXEL_FORMAT,
									 videoWidth, videoHeight, STREAM_VIDEO_PIXEL_FORMAT,
									 SWS_BILINEAR, NULL, NULL, NULL);
	if (!videoSwsContext)
		return -8;

	if (audioEnabled)
	{
		// Find audio encoder
		audioCodec = avcodec_find_encoder(outContext->oformat->audio_codec);
		if (!audioCodec)
			return -9;

		// Start audio stream
		audioStream = avformat_new_stream(outContext, audioCodec);
		if (!audioStream)
			return -10;

		audioCodecContext = audioStream->codec;
		audioStream->id = 1;

		// Setup
		audioCodecContext->sample_fmt = STREAM_AUDIO_SAMPLE_FORMAT_MOVIE;
		audioCodecContext->sample_rate = STREAM_AUDIO_SAMPLE_RATE;
		audioCodecContext->bit_rate = STREAM_AUDIO_BIT_RATE;
		audioCodecContext->channels = STREAM_AUDIO_CHANNELS;
		audioCodecContext->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;

		if (outContext->oformat->flags & AVFMT_GLOBALHEADER)
			audioCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;

		// Open the codec
		if (avcodec_open2(audioCodecContext, audioCodec, NULL) < 0)
			return -11;
	}

	// Open the output file
	if (avio_open(&outContext->pb, outFile, AVIO_FLAG_WRITE) < 0)
		return -12;

	videoFrameNum = 0;
	videoTimeBase = videoCodecContext->time_base;

	if (audioEnabled)
	{
		audioFrameNum = 0;
		audioTimeBase = rat(audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_RATE);
	}

	// Write the stream header, if any.
	if (avformat_write_header(outContext, NULL) < 0)
		return -13;

	return 0;
}

// Decode a file into raw audio
GMEXPORT double movie_audio_file_decode(const char *source, const char *dest)
{
	FILE* outStream = NULL;
	AVFormatContext* formatContext = NULL;
	AVCodec* codec = NULL;
	AVCodecContext* codecContext = NULL;
	SwrContext* swrContext = NULL;
	AVFrame* decodedFrame = NULL;
	uint8_t* convertedData = NULL;
	AVPacket inPacket;

	try
	{
		// Start output stream
		_wfopen_s(&outStream, &towstr(dest)[0], L"wb");
		if (!outStream)
			throw - 1;

		// Get format from audio file
		formatContext = avformat_alloc_context();
		if (avformat_open_input(&formatContext, source, NULL, NULL) != 0)
			throw - 2;

		if (avformat_find_stream_info(formatContext, NULL) < 0)
			throw - 3;

		// Find the first audio stream, set the codec accordingly
		codecContext = avcodec_alloc_context3(codec);
		for (unsigned int i = 0; i< formatContext->nb_streams; i++)
		{
			AVCodecParameters* par = formatContext->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				codec = avcodec_find_decoder(par->codec_id);
				if (codec == NULL)
					throw - 4;
				if (avcodec_parameters_to_context(codecContext, par) < 0)
					throw - 5;
				break;
			}
		}

		// Open codec of the audio file
		if (avcodec_open2(codecContext, codec, NULL) < 0)
			throw - 6;

		// Prepare resampling
		swrContext = swr_alloc();
		if (!swrContext)
			throw - 7;

		av_opt_set_int(swrContext, "in_channel_count", codecContext->channels, 0);
		av_opt_set_int(swrContext, "in_channel_layout", codecContext->channel_layout, 0);
		av_opt_set_int(swrContext, "in_sample_rate", codecContext->sample_rate, 0);
		av_opt_set_sample_fmt(swrContext, "in_sample_fmt", codecContext->sample_fmt, 0);

		av_opt_set_int(swrContext, "out_channel_count", STREAM_AUDIO_CHANNELS, 0);
		av_opt_set_int(swrContext, "out_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
		av_opt_set_int(swrContext, "out_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
		av_opt_set_sample_fmt(swrContext, "out_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);

		if (swr_init(swrContext))
			return -8;

		// Prepare to read data
		av_init_packet(&inPacket);
		decodedFrame = av_frame_alloc();

		// Read frames and store the decoded buffer in the resample context
		int inSamples = 0;
		while (av_read_frame(formatContext, &inPacket) >= 0)
		{
			if (avcodec_send_packet(codecContext, &inPacket) < 0 ||
				avcodec_receive_frame(codecContext, decodedFrame) < 0)
			{
				av_frame_unref(decodedFrame);
				av_packet_unref(&inPacket);
				continue;
			}

			swr_convert(swrContext, NULL, 0, (const uint8_t**)decodedFrame->data, decodedFrame->nb_samples);
			inSamples += decodedFrame->nb_samples;
		}

		// Allocate data
		if (av_samples_alloc(&convertedData, NULL, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_FRAME_SIZE, STREAM_AUDIO_SAMPLE_FORMAT_GM, 0) < 0)
			return -9;

		// Read from the resample context buffer and convert
		int samples = 0, totalSamples = (int)(inSamples / ((float)codecContext->sample_rate / (float)STREAM_AUDIO_SAMPLE_RATE));

		while (samples < totalSamples)
		{
			// Convert
			int outSamples = swr_convert(swrContext, &convertedData, STREAM_AUDIO_FRAME_SIZE, NULL, 0);
			if (outSamples < 0)
				return -10;
			samples += outSamples;

			// Calculate buffer size
			size_t bufferSize = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, outSamples, STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);
			if (bufferSize < 0)
				return -11;

			fwrite(convertedData, 1, bufferSize, outStream);
		}

		throw 0;
	}
	catch (int error)
	{
		// Clean up
		if (decodedFrame)
			av_frame_free(&decodedFrame);
		if (swrContext)
			swr_free(&swrContext);
		if (codecContext)
			avcodec_close(codecContext);
		if (formatContext) 
		{
			avformat_close_input(&formatContext);
			avformat_free_context(formatContext);
		}

		av_freep(&convertedData);
		av_packet_unref(&inPacket);

		// Close
		if (outStream)
			fclose(outStream);

		return error;
	}
}

// Adds a file with raw audio, returns the id
GMEXPORT double movie_audio_file_add(const char* source)
{
	// Create file
	File* file = new File();
	files.push_back(file);

	// Create reader
	FILE* inStream;
	_wfopen_s(&inStream, &towstr(source)[0], L"rb");

	// Calculate buffer sizes
	size_t bufferSizeGM = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);
	if (bufferSizeGM < 0)
		return -1;

	size_t bufferSizeMovie = av_samples_get_buffer_size(NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_MOVIE, 0);
	if (bufferSizeMovie < 0)
		return -2;

	// Set up resample context
	SwrContext *swrContext = swr_alloc();
	if (!swrContext)
		return -3;

	av_opt_set_int(swrContext, "in_channel_count", STREAM_AUDIO_CHANNELS, 0);
	av_opt_set_int(swrContext, "in_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
	av_opt_set_int(swrContext, "in_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
	av_opt_set_sample_fmt(swrContext, "in_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_GM, 0);

	av_opt_set_int(swrContext, "out_channel_count", STREAM_AUDIO_CHANNELS, 0);
	av_opt_set_int(swrContext, "out_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
	av_opt_set_int(swrContext, "out_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
	av_opt_set_sample_fmt(swrContext, "out_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT_MOVIE, 0);

	if (swr_init(swrContext))
		return -4;

	while (1)
	{
		uint8_t* rawData = (uint8_t*)calloc(bufferSizeGM, sizeof(uint8_t));
		if (!fread(rawData, 1, bufferSizeGM, inStream))
		{
			free(rawData);
			break;
		}

		// Convert to movie format
		uint8_t** convertedData;
		if (av_samples_alloc_array_and_samples(&convertedData, NULL, STREAM_AUDIO_CHANNELS, audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_FORMAT_MOVIE, 0) < 0)
			return -5;

		if (swr_convert(swrContext, convertedData, audioCodecContext->frame_size, (const uint8_t**)&rawData, audioCodecContext->frame_size) < 0)
			return -6;

		free(rawData);

		// Allocate frame
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return -7;

		frame->nb_samples = audioCodecContext->frame_size;
		frame->format = STREAM_AUDIO_SAMPLE_FORMAT_MOVIE;
		frame->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
		frame->channels = STREAM_AUDIO_CHANNELS;
		frame->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

		// Fill frame
		if (avcodec_fill_audio_frame(frame, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_SAMPLE_FORMAT_MOVIE, convertedData[0], bufferSizeMovie, 0) < 0)
			return -8;

		file->frames.push_back(frame);
	}

	// Close
	fclose(inStream);
	if (swrContext)
		swr_free(&swrContext);

	// Return ID
	return files.size() - 1;
}


// Adds a sound to the movie
GMEXPORT double movie_audio_sound_add(double file, double play, double volume, double start, double end)
{
	Sound* sound = new Sound();
	sound->file = files[file];
	sound->play = av_rescale_q(play * 1000, rat(1, 1000), audioTimeBase);
	sound->volume = volume;
	sound->start = av_rescale_q(start * 1000, rat(1, 1000), audioTimeBase);
	sound->end = av_rescale_q(end * 1000, rat(1, 1000), audioTimeBase);
	sounds.push_back(sound);

	return 0.0;
}


// Adds a frame to the movie
GMEXPORT double movie_frame(const char* frameFile)
{
	// Read from file
	FILE* inStream = _wfopen(&towstr(frameFile)[0], L"rb");
	int bufSize = videoWidth * videoHeight * 4;
	uint8_t* bgraData = new uint8_t[bufSize];
	fread(bgraData, 1, bufSize, inStream);
	fclose(inStream);

	// Allocate and init frame
	AVFrame *videoFrame = av_frame_alloc();
	if (!videoFrame)
		return -1;

	videoFrame->pts = 0;
	videoFrame->format = STREAM_VIDEO_PIXEL_FORMAT;
	videoFrame->width = videoWidth;
	videoFrame->height = videoHeight;

	if (av_frame_get_buffer(videoFrame, 32) < 0)
		return -2;

	// Convert
	uint8_t* inData[1] = { bgraData }; // RGBA have one plane
	int inLinesize[1] = { 4 * videoWidth }; // RGBA stride
	sws_scale(videoSwsContext, inData, inLinesize, 0, videoHeight, videoFrame->data, videoFrame->linesize);
	delete bgraData;

	// Init packet
	int gotPacket;
	AVPacket videoPacket;
	av_init_packet(&videoPacket);
	videoPacket.size = 0;
	videoPacket.data = NULL;

	// Encode the image
	videoFrame->pts = av_rescale_q(videoFrameNum, videoCodecContext->time_base, videoStream->time_base);
	if (avcodec_encode_video2(videoCodecContext, &videoPacket, videoFrame, &gotPacket) < 0)
		return -3;

	// Write the encoded frame to the media file.
	if (gotPacket)
	{
		videoPacket.stream_index = videoStream->index;

		if (av_interleaved_write_frame(outContext, &videoPacket) != 0)
			return -4;
	}

	// Free
	av_frame_free(&videoFrame);
	av_free_packet(&videoPacket);

	// Advance
	videoFrameNum++;
	
	// Write interleaved audio
	while (audioEnabled && av_compare_ts(videoFrameNum, videoTimeBase, audioFrameNum, audioTimeBase) > 0)
	{
		// Allocate frame
		AVFrame *audioFrame = av_frame_alloc();
		if (!audioFrame)
			return -5;

		audioFrame->nb_samples = audioCodecContext->frame_size;
		audioFrame->format = STREAM_AUDIO_SAMPLE_FORMAT_MOVIE;
		audioFrame->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
		audioFrame->channels = STREAM_AUDIO_CHANNELS;
		audioFrame->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

		if (av_frame_get_buffer(audioFrame, 0) < 0)
			return -6;

		if (av_frame_make_writable(audioFrame) < 0)
			return -7;

		// Find sounds
		vector<Sound*> frameSounds;
		for (size_t i = 0; i < sounds.size(); i++)
			if (audioFrameNum >= sounds[i]->play &&  audioFrameNum < sounds[i]->play + sounds[i]->file->frames.size() - sounds[i]->start + sounds[i]->end)
				frameSounds.push_back(sounds[i]);

		// Write to frame (mix sounds)
		size_t dataSize = sizeof(float);
		int isPlanar = av_sample_fmt_is_planar(STREAM_AUDIO_SAMPLE_FORMAT_MOVIE);

		for (int c = 0; c < 1 + isPlanar; c++)
		{
			for (int i = 0; i < audioFrame->linesize[0]; i += dataSize)
			{
				float dstVal = 0; // 0=silence

				for (unsigned int s = 0; s < frameSounds.size(); s++)
				{
					float srcVal;
					int fileFrame = (audioFrameNum - frameSounds[s]->play + frameSounds[s]->start) % frameSounds[s]->file->frames.size();
					memcpy(&srcVal, &frameSounds[s]->file->frames[fileFrame]->data[c][i], dataSize);

					// Clamp audio
					dstVal += srcVal * frameSounds[s]->volume;
					if (dstVal > 1)
						dstVal = 1;
					if (dstVal < -1)
						dstVal = -1;
				}

				memcpy(&audioFrame->data[c][i], &dstVal, dataSize);
			}
		}

		audioFrame->pts = av_rescale_q(audioFrameNum, audioTimeBase, audioCodecContext->time_base);

		// Allocate packet
		int gotPacket;
		AVPacket audioPacket;
		av_init_packet(&audioPacket);
		audioPacket.data = NULL;
		audioPacket.size = 0;

		// Encode
		if (avcodec_encode_audio2(audioCodecContext, &audioPacket, audioFrame, &gotPacket) < 0)
			return -8;

		// Write to file
		if (gotPacket)
		{
			av_packet_rescale_ts(&audioPacket, audioCodecContext->time_base, audioStream->time_base);
			audioPacket.stream_index = audioStream->index;

			if (av_interleaved_write_frame(outContext, &audioPacket) != 0)
				return -9;
		}

		// Free
		av_frame_free(&audioFrame);
		av_free_packet(&audioPacket);

		// Advance
		audioFrameNum++;

	}

	return 0;

}

// Finishes movie and frees memory
GMEXPORT double movie_done()
{
	// Flush video
	while (1)
	{
		int gotPacket;
		AVPacket flushPacket;
		av_init_packet(&flushPacket);
		flushPacket.data = NULL;
		flushPacket.size = 0;

		if (avcodec_encode_video2(videoCodecContext, &flushPacket, NULL, &gotPacket) < 0)
			return -1;

		if (gotPacket)
		{
			flushPacket.stream_index = videoStream->index;

			if (av_interleaved_write_frame(outContext, &flushPacket) != 0)
				return -2;
		}

		av_free_packet(&flushPacket);

		if (!gotPacket)
			break;
	}

	// Flush audio
	if (audioEnabled)
	{
		while (true)
		{
			int gotPacket;
			AVPacket flushPacket;
			av_init_packet(&flushPacket);
			flushPacket.data = NULL;
			flushPacket.size = 0;

			if (avcodec_encode_audio2(audioCodecContext, &flushPacket, NULL, &gotPacket) < 0)
				return -3;

			if (gotPacket)
			{
				flushPacket.stream_index = audioStream->index;

				if (av_interleaved_write_frame(outContext, &flushPacket) != 0)
					return -4;
			}

			av_free_packet(&flushPacket);

			if (!gotPacket)
				break;
		}
	}

	// Clear files
	for (size_t i = 0; i < files.size(); i++)
	{
		for (size_t j = 0; j < files[i]->frames.size(); j++)
		{
			av_freep(&files[i]->frames[j]->data[0]);
			av_frame_free(&files[i]->frames[j]);
		}
		delete files[i];
	}
	files.clear();

	// Clear sounds
	for (size_t i = 0; i < sounds.size(); i++)
		delete sounds[i];
	sounds.clear();

	// Write the trailer
	av_write_trailer(outContext);

	// Close video
	avcodec_close(videoCodecContext);
	sws_freeContext(videoSwsContext);

	// Close audio
	if (audioEnabled)
		avcodec_close(audioCodecContext);

	// Close the output file.
	avio_close(outContext->pb);

	// Free the stream
	avformat_free_context(outContext);

	return 0;
}