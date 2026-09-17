/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/transcription/transcription_engine.h"
#include "base/basic_types.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QThread>

#include <whisper.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>


namespace Media::Transcription {
namespace {

struct Input {
	const QByteArray &bytes;
	qint64 position = 0;

	static int Read(void *opaque, uint8_t *buffer, int size) {
		auto &input = *static_cast<Input*>(opaque);
		const auto count = std::min(qint64(size), input.bytes.size() - input.position);
		if (count <= 0) {
			return AVERROR_EOF;
		}
		std::memcpy(buffer, input.bytes.constData() + input.position, count);
		input.position += count;
		return int(count);
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		auto &input = *static_cast<Input*>(opaque);
		if (whence == AVSEEK_SIZE) {
			return input.bytes.size();
		}
		const auto base = (whence == SEEK_SET) ? qint64(0)
			: (whence == SEEK_CUR) ? input.position
			: (whence == SEEK_END) ? qint64(input.bytes.size())
			: qint64(-1);
		if (base < 0 || offset < -base || offset > input.bytes.size() - base) {
			return AVERROR(EINVAL);
		}
		return input.position = base + offset;
	}
};

struct Decoder {
	AVIOContext *io = nullptr;
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	SwrContext *resampler = nullptr;
	AVChannelLayout layout = {};
	int sampleRate = 0;
	int sampleFormat = -1;

	~Decoder() {
		av_channel_layout_uninit(&layout);
		swr_free(&resampler);
		av_packet_free(&packet);
		av_frame_free(&frame);
		avcodec_free_context(&codec);
		avformat_close_input(&format);
		if (io) {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
	}
};

} // namespace

QString ModelUrl() {
	return u"https://huggingface.co/ggerganov/whisper.cpp/resolve/"
		u"5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.bin"_q;
}

QByteArray ModelDigest() {
	return QByteArray::fromHex(
		"1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b");
}

namespace {

bool VerifyModelFile(QFile &file, const std::atomic<bool> &cancelled) {
	if (file.size() != ModelSize) {
		return false;
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		if (cancelled.load()) {
			return false;
		}
		const auto block = file.read(1024 * 1024);
		if (block.isEmpty()) {
			return false;
		}
		hash.addData(block);
	}
	return hash.result() == ModelDigest();
}

} // namespace

bool VerifyModel(const QString &path, const std::atomic<bool> &cancelled) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) && VerifyModelFile(file, cancelled);
}

Audio Decode(const QByteArray &bytes, const std::atomic<bool> &cancelled) {
	using Error = Result::Error;
	if (cancelled.load()) {
		return { {}, Error::Cancelled };
	} else if (bytes.isEmpty()) {
		return { {}, Error::Audio };
	} else if (bytes.size() > MaxAudioBytes) {
		return { {}, Error::TooLong };
	}
	auto input = Input{ bytes };
	auto decoder = Decoder();
	auto buffer = static_cast<unsigned char*>(av_malloc(4096));
	if (!buffer || !decoder.frame || !decoder.packet) {
		av_free(buffer);
		return { {}, Error::Audio };
	}
	decoder.io = avio_alloc_context(buffer, 4096, 0, &input, Input::Read, nullptr, Input::Seek);
	if (!decoder.io) {
		av_free(buffer);
		return { {}, Error::Audio };
	}
	decoder.format = avformat_alloc_context();
	if (!decoder.format) {
		return { {}, Error::Audio };
	}
	decoder.format->pb = decoder.io;
	decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
	decoder.format->io_open = [](
			AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
		return AVERROR(EACCES);
	};
	decoder.format->interrupt_callback = {
		[](void *opaque) {
			return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0;
		},
		const_cast<std::atomic<bool>*>(&cancelled),
	};
	if (av_opt_set(decoder.format, "protocol_whitelist", "", 0) < 0
		|| avformat_open_input(&decoder.format, nullptr, nullptr, nullptr) < 0
		|| avformat_find_stream_info(decoder.format, nullptr) < 0) {
		return { {}, cancelled.load() ? Error::Cancelled : Error::Audio };
	}
	const auto index = av_find_best_stream(
		decoder.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (index < 0) {
		return { {}, Error::Audio };
	}
	const auto parameters = decoder.format->streams[index]->codecpar;
	const auto codec = avcodec_find_decoder(parameters->codec_id);
	if (!codec || !(decoder.codec = avcodec_alloc_context3(codec))) {
		return { {}, Error::Audio };
	}
	decoder.codec->thread_count = 1;
	if (avcodec_parameters_to_context(decoder.codec, parameters) < 0
		|| avcodec_open2(decoder.codec, codec, nullptr) < 0) {
		return { {}, Error::Audio };
	}
	auto result = Audio();
	auto outputLayout = AVChannelLayout(AV_CHANNEL_LAYOUT_MONO);
	const auto receive = [&]() -> Error {
		while (!cancelled.load()) {
			const auto read = avcodec_receive_frame(decoder.codec, decoder.frame);
			if (read == AVERROR(EAGAIN) || read == AVERROR_EOF) {
				return Error::None;
			} else if (read < 0) {
				return Error::Audio;
			}
			const auto frame = decoder.frame;
			if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0) {
				return Error::Audio;
			}
			if (decoder.resampler && (decoder.sampleRate != frame->sample_rate
				|| decoder.sampleFormat != frame->format
				|| av_channel_layout_compare(&decoder.layout, &frame->ch_layout))) {
				return Error::Audio;
			}
			if (!decoder.resampler) {
				decoder.sampleRate = frame->sample_rate;
				decoder.sampleFormat = frame->format;
				if (av_channel_layout_copy(&decoder.layout, &frame->ch_layout) < 0) {
					return Error::Audio;
				}
				if (swr_alloc_set_opts2(&decoder.resampler,
						&outputLayout, AV_SAMPLE_FMT_FLT, SampleRate,
						&frame->ch_layout, AVSampleFormat(frame->format),
						frame->sample_rate, 0, nullptr) < 0
					|| swr_init(decoder.resampler) < 0) {
					return Error::Audio;
				}
			}
			const auto available = MaxSamples - int(result.samples.size());
			const auto capacity = std::min(available + 1,
				swr_get_out_samples(decoder.resampler, frame->nb_samples));
			if (capacity < 0) {
				return Error::Audio;
			}
			const auto offset = result.samples.size();
			result.samples.resize(offset + capacity);
			auto output = reinterpret_cast<uint8_t*>(result.samples.data() + offset);
			const auto count = swr_convert(decoder.resampler, &output, capacity,
				const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
			av_frame_unref(frame);
			if (count < 0) {
				return Error::Audio;
			} else if (count > available) {
				return Error::TooLong;
			}
			result.samples.resize(offset + count);
		}
		return Error::Cancelled;
	};
	auto read = 0;
	while (!cancelled.load() && (read = av_read_frame(decoder.format, decoder.packet)) >= 0) {
		if (decoder.packet->stream_index == index) {
			const auto sent = avcodec_send_packet(decoder.codec, decoder.packet);
			av_packet_unref(decoder.packet);
			if (sent < 0) {
				return { {}, Error::Audio };
			}
			const auto error = receive();
			if (error != Error::None) {
				return { {}, error };
			}
		} else {
			av_packet_unref(decoder.packet);
		}
	}
	if (cancelled.load()) {
		return { {}, Error::Cancelled };
	} else if (read != AVERROR_EOF || avcodec_send_packet(decoder.codec, nullptr) < 0) {
		return { {}, Error::Audio };
	}
	const auto error = receive();
	if (error != Error::None) {
		return { {}, error };
	}
	if (decoder.resampler) {
		const auto available = MaxSamples - int(result.samples.size());
		const auto capacity = std::min(available + 1,
			swr_get_out_samples(decoder.resampler, 0));
		if (capacity < 0) {
			return { {}, Error::Audio };
		}
		const auto offset = result.samples.size();
		result.samples.resize(offset + capacity);
		auto output = reinterpret_cast<uint8_t*>(result.samples.data() + offset);
		const auto count = swr_convert(decoder.resampler, &output, capacity, nullptr, 0);
		if (count < 0) {
			return { {}, Error::Audio };
		} else if (count > available) {
			return { {}, Error::TooLong };
		}
		result.samples.resize(offset + count);
	}
	return result.samples.empty() ? Audio{ {}, Error::Audio } : std::move(result);
}

Result Recognize(
		const QString &model,
		const QByteArray &audio,
		const std::atomic<bool> &cancelled,
		const std::function<void(int)> &progress) {
	using Error = Result::Error;
	auto modelFile = QFile(model);
	if (!modelFile.open(QIODevice::ReadOnly)
		|| !VerifyModelFile(modelFile, cancelled)
		|| !modelFile.seek(0)) {
		return { {}, cancelled.load() ? Error::Cancelled : Error::Model };
	}
	auto decoded = Decode(audio, cancelled);
	if (decoded.error != Error::None) {
		return { {}, decoded.error };
	}
	static auto logging = std::once_flag();
	std::call_once(logging, [] {
		whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
	});
	auto loader = whisper_model_loader{
		.context = &modelFile,
		.read = [](void *opaque, void *buffer, size_t size) {
			return size_t(std::max(qint64(0), static_cast<QFile*>(opaque)->read(
				static_cast<char*>(buffer), qint64(size))));
		},
		.eof = [](void *opaque) { return static_cast<QFile*>(opaque)->atEnd(); },
		.close = [](void*) {},
	};
	auto config = whisper_context_default_params();
	auto context = std::unique_ptr<whisper_context, decltype(&whisper_free)>(
		whisper_init_with_params(&loader, config), whisper_free);
	if (!context && config.use_gpu && !cancelled.load() && modelFile.seek(0)) {
		config.use_gpu = false;
		context.reset(whisper_init_with_params(&loader, config));
	}
	if (!context) {
		return { {}, Error::Recognition };
	}
	auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = std::clamp(QThread::idealThreadCount() / 2, 1, 8);
	params.language = "auto";
	params.translate = false;
	params.no_context = true;
	params.no_timestamps = true;
	params.print_realtime = false;
	params.print_progress = false;
	params.print_timestamps = false;
	params.suppress_nst = true;
	params.abort_callback = [](void *opaque) {
		return static_cast<const std::atomic<bool>*>(opaque)->load();
	};
	params.abort_callback_user_data = const_cast<std::atomic<bool>*>(&cancelled);
	params.progress_callback = [](whisper_context*, whisper_state*, int value, void *opaque) {
		const auto &callback = *static_cast<const std::function<void(int)>*>(opaque);
		if (callback) {
			callback(value);
		}
	};
	params.progress_callback_user_data = const_cast<std::function<void(int)>*>(&progress);
	const auto status = whisper_full(
		context.get(), params, decoded.samples.data(), decoded.samples.size());
	if (cancelled.load()) {
		return { {}, Error::Cancelled };
	} else if (status != 0) {
		return { {}, Error::Recognition };
	}
	auto text = QString();
	for (auto i = 0; i != whisper_full_n_segments(context.get()); ++i) {
		if (whisper_full_get_segment_no_speech_prob(context.get(), i) < params.no_speech_thold) {
			text += QString::fromUtf8(whisper_full_get_segment_text(context.get(), i));
		}
	}
	text = text.trimmed();
	return { text, text.isEmpty() ? Error::NoSpeech : Error::None };
}

} // namespace Media::Transcription
