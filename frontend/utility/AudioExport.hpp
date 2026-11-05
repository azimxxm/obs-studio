#pragma once

#include <QString>

#include <vector>

/* Writes finished narration to disk. Samples are interleaved float. */
namespace AudioExport {

/* plain PCM 24-bit WAV, opens in every editor */
bool WriteWav24(const QString &path, const std::vector<float> &samples, int channels, int sampleRate,
		QString &error);

/* constant bitrate MP3 through the FFmpeg libraries OBS already ships */
bool WriteMp3(const QString &path, const std::vector<float> &samples, int channels, int inputRate, int outputRate,
	      int bitrateKbps, QString &error);

} // namespace AudioExport
