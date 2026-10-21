// >>> live preview video, only runs while someone watches
#include "agent.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <media-io/video-frame.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace shy {

struct Preset {
	uint32_t width;
	int fps;
	int kbps;
	const char *name;
};
// >>> same order as the dock dropdown
static const Preset PRESETS[] = {
	{0, 0, 0, "off"},
	{640, 15, 300, "low"},
	{960, 30, 600, "medium"},
	{1280, 30, 1200, "high"},
};
static const size_t PV_MAX_SCENES = 2; // <<< each extra scene costs gpu + an encoder
// >>> server pings every 10s, no ping = nobody watching
static const auto PV_KEEPALIVE = std::chrono::seconds(25);
// >>> wait a bit before going back up
static const auto PV_CALM = std::chrono::seconds(30);

static std::atomic<int> pvQuality{2};
static std::atomic<bool> pvAutoLower{true};
static std::atomic<bool> pvLowered{false};

struct Feed {
	std::string scene;
	obs_encoder_t *enc = nullptr;
	obs_output_t *out = nullptr;
	std::atomic<bool> waitKey{true};
	// vvv picked scenes only, we draw those ourselves vvv
	obs_source_t *scene_src = nullptr;
	video_t *video = nullptr;
	gs_texrender_t *tr = nullptr;
	gs_stagesurf_t *stage = nullptr;
	bool staged = false;
	bool ticking = false;
	uint32_t w = 0, h = 0, baseW = 0, baseH = 0;
	uint32_t every = 1, tickN = 0;
};

static std::mutex pvMu;
static std::map<std::string, std::unique_ptr<Feed>> feeds;
// >>> remember what worked so we don't retry broken ones
static std::string pvEncIdTex, pvEncIdRaw;
static int pvRunningQuality = -1;
static std::vector<std::string> pvWanted;
static std::chrono::steady_clock::time_point pvLastOn;

// vvv fake output, just forwards packets to shyboti vvv
struct PvData {
	obs_output_t *output;
	Feed *feed;
};

static const char *pvName(void *)
{
	return "ShyBoti Preview";
}

static void *pvCreate(obs_data_t *settings, obs_output_t *output)
{
	return new PvData{output, (Feed *)(intptr_t)obs_data_get_int(settings, "feed")};
}

static void pvDestroy(void *data)
{
	delete static_cast<PvData *>(data);
}

static bool pvStart(void *data)
{
	auto *d = static_cast<PvData *>(data);
	if (!obs_output_can_begin_data_capture(d->output, 0))
		return false;
	if (!obs_output_initialize_encoders(d->output, 0))
		return false;
	d->feed->waitKey = true;
	return obs_output_begin_data_capture(d->output, 0);
}

static void pvStop(void *data, uint64_t)
{
	obs_output_end_data_capture(static_cast<PvData *>(data)->output);
}

// >>> header layout must match previewVideo.ts on the server
static void pvPacket(void *data, struct encoder_packet *pkt)
{
	Feed *feed = static_cast<PvData *>(data)->feed;
	if (!pkt || pkt->type != OBS_ENCODER_VIDEO)
		return;
	// >>> after a drop, wait for a keyframe or the picture breaks
	if (feed->waitKey && !pkt->keyframe)
		return;

	uint8_t *extra = nullptr;
	size_t extraSize = 0;
	if (pkt->keyframe && pkt->encoder)
		obs_encoder_get_extra_data(pkt->encoder, &extra, &extraSize);

	double ptsUs = pkt->timebase_den ? (double)pkt->pts * 1e6 * pkt->timebase_num / pkt->timebase_den : 0;
	uint16_t nameLen = (uint16_t)std::min<size_t>(feed->scene.size(), 0xffff);
	size_t head = 12 + nameLen;
	std::string buf;
	buf.resize(head + extraSize + pkt->size);
	buf[0] = 2;
	buf[1] = pkt->keyframe ? 1 : 0;
	std::memcpy(&buf[2], &ptsUs, 8); // <<< all obs platforms are little endian
	std::memcpy(&buf[10], &nameLen, 2);
	if (nameLen)
		std::memcpy(&buf[12], feed->scene.data(), nameLen);
	// >>> codec info on every keyframe, so late viewers can start
	if (extraSize)
		std::memcpy(&buf[head], extra, extraSize);
	std::memcpy(&buf[head + extraSize], pkt->data, pkt->size);

	bool sent = relaySendBinary(buf);
	if (!sent)
		feed->waitKey = true;
	else if (pkt->keyframe)
		feed->waitKey = false;
}

void previewRegister()
{
	static struct obs_output_info out = {};
	out.id = "shyboti_preview_output";
	out.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	out.get_name = pvName;
	out.create = pvCreate;
	out.destroy = pvDestroy;
	out.start = pvStart;
	out.stop = pvStop;
	out.encoded_packet = pvPacket;
	out.encoded_video_codecs = "h264";
	obs_register_output(&out);
}

// vvv encoder pick, gpu first, x264 if nothing else works vvv
// >>> gpu encoders crash on our own video (no core mix), scenes get x264 only
static std::vector<std::string> pvEncoderIds(bool raw)
{
	std::vector<std::string> hw;
	bool x264 = false;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || std::strcmp(codec, "h264") != 0)
			continue;
		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & OBS_ENCODER_CAP_DEPRECATED)
			continue;
		std::string s = id;
		if (s == "obs_x264") {
			x264 = true;
			continue;
		}
		if (!raw && (caps & OBS_ENCODER_CAP_PASS_TEXTURE) && !(caps & OBS_ENCODER_CAP_INTERNAL))
			hw.push_back(s);
	}
	if (x264)
		hw.push_back("obs_x264");
	const std::string &last = raw ? pvEncIdRaw : pvEncIdTex;
	auto it = std::find(hw.begin(), hw.end(), last);
	if (it != hw.end())
		std::rotate(hw.begin(), it, it + 1);
	return hw;
}

static obs_data_t *pvSettings(const std::string &id, const Preset &p)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", p.kbps);
	// >>> keyframe every second, scene switches don't hang long
	obs_data_set_int(s, "keyint_sec", 1);
	// >>> no b frames, every encoder names it differently
	if (id == "obs_x264") {
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "tune", "zerolatency");
		obs_data_set_string(s, "profile", "baseline");
	} else if (id.find("nvenc") != std::string::npos) {
		obs_data_set_string(s, "preset", "p1");
		obs_data_set_string(s, "tune", "ll");
		obs_data_set_int(s, "bf", 0);
	} else if (id.find("amf") != std::string::npos) {
		obs_data_set_string(s, "preset", "speed");
		obs_data_set_int(s, "bf", 0);
	} else if (id.find("qsv") != std::string::npos) {
		obs_data_set_string(s, "target_usage", "TU7");
		obs_data_set_int(s, "bframes", 0);
	} else if (id.find("videotoolbox") != std::string::npos) {
		obs_data_set_bool(s, "bframes", false);
	}
	return s;
}

// >>> grab last frame's copy, gpu is done so obs never waits
static void feedTick(void *param, float)
{
	auto *f = static_cast<Feed *>(param);
	if (++f->tickN % f->every)
		return;
	obs_enter_graphics();
	if (f->staged) {
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(f->stage, &data, &linesize)) {
			struct video_frame frame;
			if (video_output_lock_frame(f->video, &frame, 1, os_gettime_ns())) {
				uint32_t row = f->w * 4;
				for (uint32_t y = 0; y < f->h; y++)
					std::memcpy(frame.data[0] + (size_t)y * frame.linesize[0],
						    data + (size_t)y * linesize, std::min(row, frame.linesize[0]));
				video_output_unlock_frame(f->video);
			}
			gs_stagesurface_unmap(f->stage);
		}
		f->staged = false;
	}
	gs_texrender_reset(f->tr);
	if (gs_texrender_begin(f->tr, f->w, f->h)) {
		struct vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
		// >>> draws the full scene shrunk, no big render first
		gs_ortho(0.0f, (float)f->baseW, 0.0f, (float)f->baseH, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(f->scene_src);
		gs_blend_state_pop();
		gs_texrender_end(f->tr);
		gs_stage_texture(f->stage, gs_texrender_get_texture(f->tr));
		f->staged = true;
	}
	obs_leave_graphics();
}

// >>> order matters here, wrong order crashes obs
static void feedTeardown(Feed &f)
{
	using namespace std::chrono_literals;
	if (f.ticking) {
		obs_remove_tick_callback(feedTick, &f);
		f.ticking = false;
	}
	if (f.out) {
		obs_output_stop(f.out);
		for (int i = 0; i < 40 && obs_output_active(f.out); i++)
			std::this_thread::sleep_for(50ms);
		obs_output_release(f.out);
		f.out = nullptr;
	}
	if (f.enc) {
		for (int i = 0; i < 40 && obs_encoder_active(f.enc); i++)
			std::this_thread::sleep_for(50ms);
		obs_encoder_release(f.enc);
		f.enc = nullptr;
	}
	if (f.video) {
		video_output_close(f.video);
		f.video = nullptr;
	}
	if (f.tr || f.stage) {
		obs_enter_graphics();
		gs_texrender_destroy(f.tr);
		gs_stagesurface_destroy(f.stage);
		obs_leave_graphics();
		f.tr = nullptr;
		f.stage = nullptr;
	}
	if (f.scene_src) {
		obs_source_dec_showing(f.scene_src);
		obs_source_release(f.scene_src);
		f.scene_src = nullptr;
	}
}

static bool feedStart(Feed &f, const std::string &encId, const Preset &p)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return false;
	uint32_t w = std::min<uint32_t>(p.width, ovi.base_width);
	// >>> encoders want even sizes
	uint32_t h = (uint32_t)std::lround((double)ovi.base_height * w / ovi.base_width) & ~1u;
	w &= ~1u;
	double fps = ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30;
	uint32_t div = (uint32_t)std::max(1L, std::lround(fps / p.fps));

	video_t *video = obs_get_video();
	if (!f.scene.empty()) {
		obs_source_t *scene = obs_get_source_by_name(f.scene.c_str());
		if (!scene)
			return false;
		if (!obs_source_is_scene(scene)) {
			obs_source_release(scene);
			return false;
		}
		// >>> "showing" not "active", so media doesn't restart
		obs_source_inc_showing(scene);
		f.scene_src = scene;
		f.w = w;
		f.h = h;
		f.baseW = ovi.base_width;
		f.baseH = ovi.base_height;
		f.every = div;

		struct video_output_info voi = {};
		voi.name = "shyboti-preview";
		voi.format = VIDEO_FORMAT_RGBA; // <<< obs converts this on another thread
		voi.fps_num = ovi.fps_num;
		voi.fps_den = ovi.fps_den * div;
		voi.width = w;
		voi.height = h;
		voi.cache_size = 4;
		voi.colorspace = VIDEO_CS_709;
		voi.range = VIDEO_RANGE_PARTIAL;
		if (video_output_open(&f.video, &voi) != VIDEO_OUTPUT_SUCCESS) {
			f.video = nullptr;
			return false;
		}
		video = f.video;

		obs_enter_graphics();
		f.tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f.stage = gs_stagesurface_create(w, h, GS_RGBA);
		obs_leave_graphics();
		if (!f.tr || !f.stage)
			return false;
	}

	obs_data_t *s = pvSettings(encId, p);
	f.enc = obs_video_encoder_create(encId.c_str(), "shyboti-preview-enc", s, nullptr);
	obs_data_release(s);
	if (!f.enc)
		return false;
	obs_encoder_set_video(f.enc, video);
	if (f.scene.empty()) {
		// >>> scaled on the gpu, stream stays untouched
		obs_encoder_set_gpu_scale_type(f.enc, OBS_SCALE_BICUBIC);
		obs_encoder_set_scaled_size(f.enc, w, h);
		if (div > 1)
			obs_encoder_set_frame_rate_divisor(f.enc, div);
	}

	obs_data_t *os = obs_data_create();
	obs_data_set_int(os, "feed", (long long)(intptr_t)&f);
	f.out = obs_output_create("shyboti_preview_output", "shyboti-preview", os, nullptr);
	obs_data_release(os);
	if (!f.out)
		return false;
	obs_output_set_video_encoder(f.out, f.enc);
	if (!obs_output_start(f.out))
		return false;
	// >>> start drawing only once the encoder is ready
	if (f.video) {
		obs_add_tick_callback(feedTick, &f);
		f.ticking = true;
	}
	return true;
}

static bool feedRunning(const Feed &f)
{
	return f.out && obs_output_active(f.out);
}

// >>> stream struggling = everything drops to low
static int effectiveQuality()
{
	int q = pvQuality;
	return q > 1 && pvAutoLower && pvLowered ? 1 : q;
}

static void teardownAllLocked()
{
	for (auto &[_, f] : feeds)
		feedTeardown(*f);
	feeds.clear();
}

// >>> scenes past the cap just get skipped
static json applyLocked(const std::vector<std::string> &wanted)
{
	int q = effectiveQuality();
	json status = {{"quality", PRESETS[q].name}, {"lowered", q != pvQuality.load()}};
	// >>> can't resize a running encoder, restart all
	if (q != pvRunningQuality)
		teardownAllLocked();
	pvRunningQuality = q;
	if (q == 0) {
		pvWanted = wanted; // <<< so turning it back on is instant
		status["running"] = json::array();
		return status;
	}
	const Preset &p = PRESETS[q];

	std::vector<std::string> want;
	size_t sceneCount = 0;
	for (auto &s : wanted) {
		if (std::find(want.begin(), want.end(), s) != want.end())
			continue;
		if (!s.empty() && sceneCount >= PV_MAX_SCENES)
			continue;
		if (!s.empty())
			sceneCount++;
		want.push_back(s);
	}
	pvWanted = want;

	for (auto it = feeds.begin(); it != feeds.end();) {
		bool keep = std::find(want.begin(), want.end(), it->first) != want.end() && feedRunning(*it->second);
		if (keep) {
			++it;
			continue;
		}
		feedTeardown(*it->second);
		it = feeds.erase(it);
	}

	json running = json::array();
	for (auto &s : want) {
		if (feeds.count(s)) {
			running.push_back(s);
			continue;
		}
		bool raw = !s.empty();
		for (auto &id : pvEncoderIds(raw)) {
			auto f = std::make_unique<Feed>();
			f->scene = s;
			if (feedStart(*f, id, p)) {
				std::string &last = raw ? pvEncIdRaw : pvEncIdTex;
				if (last != id)
					log("Live preview uses " + id + ".");
				last = id;
				feeds[s] = std::move(f);
				running.push_back(s);
				break;
			}
			feedTeardown(*f);
		}
	}
	status["running"] = running;
	status["encoder"] = pvEncIdTex;
	return status;
}

json previewSet(const std::vector<std::string> &wanted)
{
	std::lock_guard<std::mutex> l(pvMu);
	pvLastOn = std::chrono::steady_clock::now();
	return applyLocked(wanted);
}

void previewOff()
{
	std::lock_guard<std::mutex> l(pvMu);
	teardownAllLocked();
	pvWanted.clear();
	pvRunningQuality = -1;
}

void previewConfigure(int quality, bool autoLower)
{
	pvQuality = std::max(0, std::min(3, quality));
	pvAutoLower = autoLower;
}

// >>> called every few seconds from the bitrate check
void previewNoteCongestion(bool congested)
{
	static std::chrono::steady_clock::time_point lastBad;
	auto now = std::chrono::steady_clock::now();
	if (congested) {
		lastBad = now;
		if (!pvLowered.exchange(true) && pvAutoLower && pvQuality > 1)
			log("Stream is struggling, live preview dropped to low.");
	} else if (pvLowered && now - lastBad > PV_CALM) {
		pvLowered = false;
	}
}

void previewTick()
{
	std::lock_guard<std::mutex> l(pvMu);
	if (pvWanted.empty())
		return;
	if (std::chrono::steady_clock::now() - pvLastOn >= PV_KEEPALIVE) {
		teardownAllLocked();
		pvWanted.clear();
		pvRunningQuality = -1;
		return;
	}
	// >>> quality changed in the dock or by congestion
	if (effectiveQuality() != pvRunningQuality)
		applyLocked(std::vector<std::string>(pvWanted));
}

} // namespace shy
