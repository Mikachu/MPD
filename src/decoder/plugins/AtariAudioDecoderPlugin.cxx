// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include <cstdlib>
#include <cassert>
#include <forward_list>
#include <string>

#include "AtariAudioDecoderPlugin.hxx"
#include "decoder/Features.h"
#include "../DecoderAPI.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/NarrowPath.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "lib/fmt/PathFormatter.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "io/FileReader.hxx"
#include "util/AllocatedArray.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringCompare.hxx"
#include "config/Block.hxx"

#include <atariaudiorenderer.h>

#include <fmt/format.h>

#define SUBTUNE_PREFIX "tune_"

/*
 * The Atari ST family of computers has a 24-bit bus,
 * so 16 MiB is more than enough for any SNDH/YM file.
 */
#define MAX_FILE_SIZE 0x1000000

struct AtariAudioGlobal {
	unsigned default_songlength;
	std::string default_genre;

	explicit AtariAudioGlobal(const ConfigBlock &block);
};

static AtariAudioGlobal *atariaudio_global;

inline
AtariAudioGlobal::AtariAudioGlobal(const ConfigBlock &block)
{
	default_songlength = block.GetPositiveValue("default_songlength", 0U);

	default_genre = block.GetBlockValue("default_genre", "");
}

static bool
atariaudio_init(const ConfigBlock &block)
{
	atariaudio_global = new AtariAudioGlobal(block);
	return true;
}

static void
atariaudio_finish() noexcept
{
	delete atariaudio_global;
}

struct AtariAudioContainerPath {
	AllocatedPath path;
	unsigned track;
};

static AllocatedArray<std::byte>
atariaudio_read_file(const Path &path_fs)
{
	FileReader file(path_fs);

	const size_t size = file.GetSize();

	if (size > MAX_FILE_SIZE)
		throw FmtRuntimeError("File larger than {} bytes: {}",
				      MAX_FILE_SIZE, path_fs);

	AllocatedArray<std::byte> data(size);

	file.ReadFull(data);

	return data;
}

[[gnu::pure]]
static unsigned
atariaudio_subtune_track(const char *base) noexcept
{
	base = StringAfterPrefix(base, SUBTUNE_PREFIX);
	if (base == nullptr)
		return 0;

	char *endptr;
	auto track = strtoul(base, &endptr, 10);
	if (endptr == base || *endptr != '.')
		return 0;

	return track;
}

/**
 * Returns the file path stripped of any /tune_xxx.* subtune suffix
 * and the track number (or 1 if no "tune_xxx" suffix is present).
 */
static AtariAudioContainerPath
atariaudio_container_from_path(Path path_fs) noexcept
{
	const NarrowPath base = NarrowPath(path_fs.GetBase());
	unsigned track;
	if (!base || (track = atariaudio_subtune_track(base)) < 1)
		return { AllocatedPath(path_fs), 1 };

	return { path_fs.GetDirectoryName(), track };
}

/**
 * Creates a renderer for the given file data, or nullptr if the
 * data was not recognized as a .sndh or .ym file.
 */
static AtariAudioRenderer *
atariaudio_create(const AllocatedArray<std::byte> &tune,
		  uint32_t sample_rate) noexcept
{
	auto *ar = AtariAudioRenderer::Create(tune.data(), tune.size(),
					      sample_rate);
	if (ar == nullptr)
		return nullptr;

	if (ar->GetSongInfo().fileType ==
	    AtariAudioRenderer::eFileType::eUnknown) {
		AtariAudioRenderer::Destroy(ar);
		return nullptr;
	}

	return ar;
}

/**
 * Returns the subsong duration in output samples, falling back to
 * the configured default_songlength when the file carries no
 * duration information.  Returns 0 if neither is available.
 */
static uint64_t
atariaudio_subtune_frames(AtariAudioRenderer &ar, int subsong,
			  uint32_t sample_rate) noexcept
{
	uint64_t frames = ar.GetSubsongDurationSample(subsong);
	if (frames == 0)
		frames = uint64_t(atariaudio_global->default_songlength) *
			sample_rate;
	return frames;
}

static void
atariaudio_file_decode(DecoderClient &client, Path path_fs)
{
	/* AtariAudio renders mono 16-bit samples */
	static constexpr AudioFormat audio_format(48000, SampleFormat::S16, 1);
	assert(audio_format.IsValid());

	const auto container = atariaudio_container_from_path(path_fs);

	const AllocatedArray<std::byte> tune = atariaudio_read_file(container.path);
	if (tune.empty())
		return;

	auto *ar = atariaudio_create(tune, audio_format.sample_rate);
	if (ar == nullptr)
		return;

	AtScopeExit(&ar) { AtariAudioRenderer::Destroy(ar); };

	/* reject bogus track numbers parsed from the filename before
	   they can index the library's internal subsong table */
	if (container.track > unsigned(ar->GetSongInfo().subsongCount))
		return;

	/* exact duration in samples; 0 means "unknown" */
	const uint64_t end_frames = 
		atariaudio_subtune_frames(*ar, container.track,
					  audio_format.sample_rate);		

	const SignedSongTime duration = end_frames > 0
		? SignedSongTime::FromScale(end_frames, audio_format.sample_rate)
		: SignedSongTime::Negative();

	/* subsongs start at 1 by convention; required even for .ym */
	if (!ar->InitSubSong(container.track))
		return;

	client.Ready(audio_format, true, duration);

	uint64_t t_frames = 0;
	DecoderCommand cmd;

	do {
		enum { N = 4096 };
		int16_t buffer[N];

		uint32_t n_frames = N;
		if (end_frames > 0 && t_frames + n_frames > end_frames)
			n_frames = end_frames - t_frames;
		if (n_frames == 0)
			break;

		/* AudioRender never signals EOF; songs loop forever */
		ar->AudioRender(buffer, n_frames);

		cmd = client.SubmitAudio(nullptr,
					 std::span{buffer, (size_t)n_frames},
					 0);
		t_frames += n_frames;

		if (cmd == DecoderCommand::SEEK) {
			uint64_t s_frames = client.GetSeekFrame();

			/* clamp seeks past the end of the song */
			if (end_frames > 0 && s_frames > end_frames)
				s_frames = end_frames;

			if (s_frames < t_frames) {
				/* no backward seek: restart the song */
				AtariAudioRenderer::Destroy(ar);

				ar = atariaudio_create(tune,
						       audio_format.sample_rate);
				if (ar == nullptr)
					return;
				if (!ar->InitSubSong(container.track))
					return;

				t_frames = 0;
			}

			if (s_frames > t_frames) {
				/* full emulation without audio output */
				ar->FastForward(s_frames - t_frames);
				t_frames = s_frames;
			}

			client.CommandFinished();
		}
	} while (cmd != DecoderCommand::STOP);
}

static void
atariaudio_on_tag(unsigned track, TagHandler &th,
		  AtariAudioRenderer &ar,
		  uint32_t sample_rate) noexcept
{
	const auto &info = ar.GetSongInfo();

	if (*info.musicName)
		th.OnTag(TAG_TITLE, info.musicName);
	if (*info.musicAuthor)
		th.OnTag(TAG_ARTIST, info.musicAuthor);
	if (*info.year)
		th.OnTag(TAG_DATE, info.year);
	if (*info.fileFormat)
		th.OnTag(TAG_COMMENT, info.fileFormat);

	if (!atariaudio_global->default_genre.empty())
		th.OnTag(TAG_GENRE,
			 atariaudio_global->default_genre.c_str());

	const uint64_t frames = atariaudio_subtune_frames(ar, track, sample_rate);
	if (frames > 0)
		th.OnDuration(SongTime::Cast(
			SignedSongTime::FromScale(frames, sample_rate)));

	if (info.subsongCount > 1)
		th.OnTag(TAG_TRACK, fmt::format_int{track}.c_str());
}

static bool
atariaudio_scan_file(Path path_fs, TagHandler &th)
{
	static constexpr uint32_t sample_rate = 48000;

	const auto container = atariaudio_container_from_path(path_fs);

	const AllocatedArray<std::byte> tune = atariaudio_read_file(container.path);
	if (tune.empty())
		return false;

	auto *ar = atariaudio_create(tune, sample_rate);
	if (ar == nullptr)
		return false;

	AtScopeExit(ar) { AtariAudioRenderer::Destroy(ar); };

	if (container.track > unsigned(ar->GetSongInfo().subsongCount))
		return false;

	atariaudio_on_tag(container.track, th, *ar, sample_rate);

	return true;
}

static std::forward_list<DetachedSong>
atariaudio_container_scan(Path path_fs)
{
	static constexpr uint32_t sample_rate = 48000;

	std::forward_list<DetachedSong> list;

	const AllocatedArray<std::byte> tune = atariaudio_read_file(path_fs);
	if (tune.empty())
		return list;

	auto *ar = atariaudio_create(tune, sample_rate);
	if (ar == nullptr)
		return list;

	AtScopeExit(ar) { AtariAudioRenderer::Destroy(ar); };

	const int n_tracks = ar->GetSongInfo().subsongCount;

	/* single-subsong files (always the case for .ym) are
	   played as normal files, not containers */
	if (n_tracks <= 1)
		return list;

	/* reuse the container file's extension so the virtual
	   paths are routed back to this plugin */
	const char *const ext = path_fs.GetExtension();
	if (ext == nullptr)
		return list;

	TagBuilder tag_builder;

	auto tail = list.before_begin();
	for (int i = 1; i <= n_tracks; ++i) {
		AddTagHandler th(tag_builder);

		atariaudio_on_tag(i, th, *ar, sample_rate);

		/* Construct container/tune path names, for example
		   Delta.sndh/tune_001.sndh */
		tail = list.emplace_after(tail,
					  fmt::format(SUBTUNE_PREFIX "{:03}.{}", i, ext),
					  tag_builder.Commit());
	}

	return list;
}

static constexpr const char *atariaudio_suffixes[] = {
	"sndh",
	"ym",
	"ymt",
	nullptr
};

constexpr DecoderPlugin atariaudio_decoder_plugin =
	DecoderPlugin("atariaudio", atariaudio_file_decode, atariaudio_scan_file)
	.WithInit(atariaudio_init, atariaudio_finish)
	.WithContainer(atariaudio_container_scan)
	.WithSuffixes(atariaudio_suffixes);
