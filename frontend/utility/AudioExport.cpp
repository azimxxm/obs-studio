#include "AudioExport.hpp"

#include <algorithm>
#include <cmath>

#include <QByteArray>
#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace AudioExport {

namespace {
constexpr int kBytesPerSample = 3;
constexpr int kChunkFrames = 4096;

void WriteLE32(QByteArray &out, quint32 v)
{
	char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
	out.append(b, 4);
}

void WriteLE16(QByteArray &out, quint16 v)
{
	char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
	out.append(b, 2);
}

QString AvError(int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, text, sizeof(text));
	return QString::fromUtf8(text);
}

/* frees everything the MP3 writer opened, on every exit path */
struct Mp3Writer {
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	SwrContext *resampler = nullptr;
	AVAudioFifo *fifo = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	AVStream *stream = nullptr;
	int64_t pts = 0;

	~Mp3Writer()
	{
		av_packet_free(&packet);
		av_frame_free(&frame);
		if (fifo) {
			av_audio_fifo_free(fifo);
		}
		swr_free(&resampler);
		avcodec_free_context(&codec);
		if (format) {
			if (format->pb) {
				avio_closep(&format->pb);
			}
			avformat_free_context(format);
		}
	}

	/* send one frame (or nullptr to flush) and write whatever comes out */
	int Encode(AVFrame *input)
	{
		int ret = avcodec_send_frame(codec, input);
		if (ret < 0) {
			return ret;
		}
		while ((ret = avcodec_receive_packet(codec, packet)) >= 0) {
			av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
			packet->stream_index = stream->index;
			ret = av_interleaved_write_frame(format, packet);
			if (ret < 0) {
				return ret;
			}
		}
		return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
	}

	/* encode full frames from the fifo; pad the last one with silence when flushing */
	int Drain(bool flush)
	{
		const int frameSize = codec->frame_size;
		while (av_audio_fifo_size(fifo) >= frameSize || (flush && av_audio_fifo_size(fifo) > 0)) {
			frame->nb_samples = frameSize;
			int ret = av_frame_make_writable(frame);
			if (ret < 0) {
				return ret;
			}
			int got = av_audio_fifo_read(fifo, reinterpret_cast<void **>(frame->data), frameSize);
			if (got < frameSize) {
				av_samples_set_silence(frame->data, got, frameSize - got, codec->ch_layout.nb_channels,
						       codec->sample_fmt);
			}
			frame->pts = pts;
			pts += frameSize;
			ret = Encode(frame);
			if (ret < 0) {
				return ret;
			}
		}
		return 0;
	}
};
} // namespace

bool WriteWav24(const QString &path, const std::vector<float> &samples, int channels, int sampleRate,
		QString &error)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		error = file.errorString();
		return false;
	}

	const quint32 dataBytes = quint32(samples.size() * kBytesPerSample);
	const quint16 blockAlign = quint16(channels * kBytesPerSample);
	QByteArray header;
	header.append("RIFF", 4);
	WriteLE32(header, 36 + dataBytes);
	header.append("WAVEfmt ", 8);
	WriteLE32(header, 16);
	WriteLE16(header, 1);
	WriteLE16(header, quint16(channels));
	WriteLE32(header, quint32(sampleRate));
	WriteLE32(header, quint32(sampleRate) * blockAlign);
	WriteLE16(header, blockAlign);
	WriteLE16(header, kBytesPerSample * 8);
	header.append("data", 4);
	WriteLE32(header, dataBytes);
	file.write(header);

	constexpr size_t kChunkSamples = 1 << 16;
	QByteArray pcm;
	for (size_t start = 0; start < samples.size(); start += kChunkSamples) {
		size_t count = std::min(kChunkSamples, samples.size() - start);
		pcm.resize(qsizetype(count * kBytesPerSample));
		char *dst = pcm.data();
		for (size_t i = start; i < start + count; i++) {
			float clamped = std::clamp(samples[i], -1.0f, 1.0f);
			qint32 value = qint32(std::lround(clamped * 8388607.0f));
			*dst++ = char(value & 0xff);
			*dst++ = char((value >> 8) & 0xff);
			*dst++ = char((value >> 16) & 0xff);
		}
		if (file.write(pcm) != pcm.size()) {
			error = file.errorString();
			return false;
		}
	}
	return true;
}

bool WriteMp3(const QString &path, const std::vector<float> &samples, int channels, int inputRate, int outputRate,
	      int bitrateKbps, QString &error)
{
	Mp3Writer w;
	const QByteArray utf8Path = path.toUtf8();

	int ret = avformat_alloc_output_context2(&w.format, nullptr, "mp3", utf8Path.constData());
	if (ret < 0 || !w.format) {
		error = AvError(ret);
		return false;
	}

	const AVCodec *encoder = avcodec_find_encoder_by_name("libmp3lame");
	if (!encoder) {
		error = QStringLiteral("The MP3 encoder (LAME) is not available.");
		return false;
	}
	w.codec = avcodec_alloc_context3(encoder);
	w.codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	w.codec->sample_rate = outputRate;
	w.codec->bit_rate = int64_t(bitrateKbps) * 1000;
	w.codec->time_base = AVRational{1, outputRate};
	av_channel_layout_default(&w.codec->ch_layout, channels);
	if (w.format->oformat->flags & AVFMT_GLOBALHEADER) {
		w.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	if ((ret = avcodec_open2(w.codec, encoder, nullptr)) < 0) {
		error = AvError(ret);
		return false;
	}

	w.stream = avformat_new_stream(w.format, nullptr);
	avcodec_parameters_from_context(w.stream->codecpar, w.codec);
	w.stream->time_base = w.codec->time_base;

	if ((ret = avio_open(&w.format->pb, utf8Path.constData(), AVIO_FLAG_WRITE)) < 0 ||
	    (ret = avformat_write_header(w.format, nullptr)) < 0) {
		error = AvError(ret);
		return false;
	}

	AVChannelLayout inputLayout;
	av_channel_layout_default(&inputLayout, channels);
	ret = swr_alloc_set_opts2(&w.resampler, &w.codec->ch_layout, w.codec->sample_fmt, outputRate, &inputLayout,
				  AV_SAMPLE_FMT_FLT, inputRate, 0, nullptr);
	av_channel_layout_uninit(&inputLayout);
	if (ret < 0 || (ret = swr_init(w.resampler)) < 0) {
		error = AvError(ret);
		return false;
	}

	w.fifo = av_audio_fifo_alloc(w.codec->sample_fmt, channels, w.codec->frame_size * 4);
	w.frame = av_frame_alloc();
	w.packet = av_packet_alloc();
	w.frame->format = w.codec->sample_fmt;
	w.frame->sample_rate = outputRate;
	av_channel_layout_copy(&w.frame->ch_layout, &w.codec->ch_layout);
	w.frame->nb_samples = w.codec->frame_size;
	if (!w.fifo || av_frame_get_buffer(w.frame, 0) < 0) {
		error = QStringLiteral("Out of memory.");
		return false;
	}

	/* resample in chunks into the fifo, then encode whole MP3 frames */
	const int64_t frames = int64_t(samples.size()) / channels;
	const int maxOut = int(av_rescale_rnd(kChunkFrames, outputRate, inputRate, AV_ROUND_UP)) + 64;
	uint8_t **converted = nullptr;
	av_samples_alloc_array_and_samples(&converted, nullptr, channels, maxOut, w.codec->sample_fmt, 0);

	bool ok = true;
	for (int64_t start = 0; ok && start <= frames; start += kChunkFrames) {
		/* the last round passes no input, which flushes the resampler */
		int count = int(std::min<int64_t>(kChunkFrames, frames - start));
		const uint8_t *input = count > 0 ? reinterpret_cast<const uint8_t *>(samples.data() + start * channels)
						 : nullptr;
		int out = swr_convert(w.resampler, converted, maxOut, input ? &input : nullptr, count > 0 ? count : 0);
		if (out < 0) {
			error = AvError(out);
			ok = false;
			break;
		}
		if (out > 0 && av_audio_fifo_write(w.fifo, reinterpret_cast<void **>(converted), out) < out) {
			error = QStringLiteral("Out of memory.");
			ok = false;
			break;
		}
		if ((ret = w.Drain(false)) < 0) {
			error = AvError(ret);
			ok = false;
		}
		if (count <= 0) {
			break;
		}
	}
	if (converted) {
		av_freep(&converted[0]);
		av_freep(&converted);
	}
	if (!ok) {
		return false;
	}

	if ((ret = w.Drain(true)) < 0 || (ret = w.Encode(nullptr)) < 0 || (ret = av_write_trailer(w.format)) < 0) {
		error = AvError(ret);
		return false;
	}
	return true;
}

} // namespace AudioExport
