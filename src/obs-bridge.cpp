// >>> port of localObs.js, same requests, same answers
#include "agent.hpp"

#include <obs-module.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <util/platform.h>

#include <ixwebsocket/IXHttpClient.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace shy {

// vvv obs-websocket in-process api, no network, no port vvv
// >>> same layout as obs-websocket's lib/obs-websocket-api.h (GPL-2.0)
typedef void (*ws_event_cb)(uint64_t, const char *, const char *, void *);
struct ws_event_callback {
	ws_event_cb callback;
	void *priv_data;
};
struct ws_request_response {
	unsigned int status_code;
	char *comment;
	char *response_data;
};

static proc_handler_t *wsPh()
{
	static proc_handler_t *ph = nullptr;
	if (ph)
		return ph;
	calldata_t cd = {0, 0, 0, 0};
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd))
		ph = (proc_handler_t *)calldata_ptr(&cd, "ph");
	calldata_free(&cd);
	return ph;
}

bool obsApiReady()
{
	return wsPh() != nullptr;
}

json obsRequest(const std::string &type, const json &data)
{
	proc_handler_t *ph = wsPh();
	if (!ph)
		throw std::runtime_error("obs-websocket is missing in this OBS");
	// >>> raw json string, obs_data would mangle arrays
	std::string body = data.is_object() ? dumpJson(data) : "";
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "request_type", type.c_str());
	calldata_set_string(&cd, "request_data", body.empty() ? nullptr : body.c_str());
	proc_handler_call(ph, "call_request", &cd);
	auto *r = (ws_request_response *)calldata_ptr(&cd, "response");
	calldata_free(&cd);
	if (!r)
		throw std::runtime_error(type + " failed");
	unsigned int code = r->status_code;
	std::string comment = r->comment ? r->comment : "";
	std::string out = r->response_data ? r->response_data : "";
	if (r->comment)
		bfree(r->comment);
	if (r->response_data)
		bfree(r->response_data);
	bfree(r);
	if (code != 100)
		throw std::runtime_error(comment.empty() ? type + " failed (" + std::to_string(code) + ")" : comment);
	if (out.empty())
		return json::object();
	json j = json::parse(out, nullptr, false);
	return j.is_discarded() ? json::object() : j;
}

// vvv events -> relay vvv
static std::mutex evMu;
static std::set<std::string> itemScenes;
static std::chrono::steady_clock::time_point itemsDue;
static bool itemsPending = false;
static const std::regex ITEM_EVENTS("^(SceneItem|InputSettingsChanged|InputNameChanged|InputCreated|InputRemoved)");

static void onWsEvent(uint64_t, const char *type, const char *data, void *)
{
	std::string t = type ? type : "";
	if (t == "CurrentProgramSceneChanged") {
		json d = json::parse(data ? data : "{}", nullptr, false);
		relaySendEvent(t, {{"sceneName", jstr(d, "sceneName")}});
		return;
	}
	if (!std::regex_search(t, ITEM_EVENTS))
		return;
	// >>> transform events fire per frame while dragging, batch 400ms
	json d = json::parse(data ? data : "{}", nullptr, false);
	std::lock_guard<std::mutex> l(evMu);
	itemScenes.insert(jstr(d, "sceneName"));
	if (!itemsPending) {
		itemsPending = true;
		itemsDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
	}
}

void obsEventsTick()
{
	std::vector<std::string> scenes;
	{
		std::lock_guard<std::mutex> l(evMu);
		if (!itemsPending || std::chrono::steady_clock::now() < itemsDue)
			return;
		itemsPending = false;
		scenes.assign(itemScenes.begin(), itemScenes.end());
		itemScenes.clear();
	}
	relaySendEvent("SceneItemsChanged", {{"scenes", scenes}});
}

static bool eventsOn = false;

void obsEventsStart()
{
	proc_handler_t *ph = wsPh();
	if (!ph || eventsOn)
		return;
	ws_event_callback cb = {onWsEvent, nullptr};
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_ptr(&cd, "callback", &cb);
	proc_handler_call(ph, "register_event_callback", &cd);
	eventsOn = calldata_bool(&cd, "success");
	calldata_free(&cd);
	if (!eventsOn)
		log("Could not subscribe to OBS events.");
}

void obsEventsStop()
{
	proc_handler_t *ph = wsPh();
	if (!ph || !eventsOn)
		return;
	ws_event_callback cb = {onWsEvent, nullptr};
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_ptr(&cd, "callback", &cb);
	proc_handler_call(ph, "unregister_event_callback", &cd);
	calldata_free(&cd);
	eventsOn = false;
}

// vvv helpers vvv
static const std::regex SETTINGS_KINDS("^(text_gdiplus|text_ft2_source|browser_source|image_source|ffmpeg_source|color_source)");
static const std::vector<std::string> TEXT_KINDS = {"text_gdiplus_v3", "text_gdiplus_v2", "text_gdiplus",
						    "text_ft2_source_v2", "text_ft2_source"};
static const std::vector<std::string> COLOR_KINDS = {"color_source_v3", "color_source_v2", "color_source"};
static const std::set<std::string> AUDIO_KINDS = {
	"wasapi_input_capture",  "wasapi_output_capture", "wasapi_process_output_capture",
	"coreaudio_input_capture", "coreaudio_output_capture", "pulse_input_capture",
	"pulse_output_capture",  "ffmpeg_source",         "browser_source",
	"vlc_source"};
static const std::regex IMAGE_EXT("\\.(png|jpe?g|gif|webp|bmp|svg)$", std::regex::icase);
static const size_t MAX_FILE_BYTES = 15 * 1024 * 1024;

static bool startsWith(const std::string &s, const char *p)
{
	return s.rfind(p, 0) == 0;
}

static json field(const json &j, const char *key)
{
	if (!j.is_object())
		return json();
	auto it = j.find(key);
	return it == j.end() ? json() : *it;
}

static json arr(const json &j, const char *key)
{
	json v = field(j, key);
	return v.is_array() ? v : json::array();
}

static std::string base64(const std::string &in)
{
	static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		unsigned n = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
		out += T[(n >> 18) & 63];
		out += T[(n >> 12) & 63];
		out += T[(n >> 6) & 63];
		out += T[n & 63];
	}
	if (i < in.size()) {
		unsigned n = (unsigned char)in[i] << 16;
		if (i + 1 < in.size())
			n |= (unsigned char)in[i + 1] << 8;
		out += T[(n >> 18) & 63];
		out += T[(n >> 12) & 63];
		out += i + 1 < in.size() ? T[(n >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

static std::string hashName(const std::string &s)
{
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	char buf[17];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
	return buf;
}

// >>> u8string is std::u8string from c++20 on
static std::string u8(const fs::path &p)
{
#if defined(__cpp_char8_t)
	auto s = p.u8string();
	return std::string(s.begin(), s.end());
#else
	return p.u8string();
#endif
}

static std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// >>> obs image_source wants a local file, urls get cached here
static std::string downloadToMedia(const std::string &url)
{
	if (!startsWith(url, "https://") && !startsWith(url, "http://"))
		throw std::runtime_error("bad url");
	ix::HttpClient client;
	auto args = client.createRequest();
	args->connectTimeout = 10;
	args->transferTimeout = 15;
	args->followRedirects = true;
	args->maxRedirects = 3;
	auto res = client.get(url, args);
	if (!res || res->statusCode != 200)
		throw std::runtime_error("download failed (" + std::to_string(res ? res->statusCode : 0) + ")");
	if (res->body.size() > MAX_FILE_BYTES)
		throw std::runtime_error("file too big");

	std::smatch m;
	std::string path = url.substr(0, url.find_first_of("?#"));
	std::string ext;
	if (std::regex_search(path, m, IMAGE_EXT))
		ext = lower(m.str(0));
	if (ext.empty()) {
		std::string type;
		auto it = res->headers.find("Content-Type");
		if (it != res->headers.end())
			type = lower(it->second.substr(0, it->second.find(';')));
		ext = type == "image/jpeg"	? ".jpg"
		      : type == "image/gif"	? ".gif"
		      : type == "image/webp"	? ".webp"
		      : type == "image/svg+xml" ? ".svg"
						: ".png";
	}
	std::string dir = configPath("media");
	os_mkdirs(dir.c_str());
	std::string file = dir + "/" + hashName(url) + ext;
	std::ofstream f(fs::u8path(file), std::ios::binary | std::ios::trunc);
	if (!f)
		throw std::runtime_error("could not save the file");
	f.write(res->body.data(), (std::streamsize)res->body.size());
	return file;
}

static std::set<std::string> kindList;
static std::mutex kindMu;

static std::string pickKind(const std::vector<std::string> &candidates)
{
	{
		std::lock_guard<std::mutex> l(kindMu);
		if (kindList.empty()) {
			json d = obsRequest("GetInputKindList", {{"unversioned", false}});
			for (auto &k : arr(d, "inputKinds"))
				if (k.is_string())
					kindList.insert(k.get<std::string>());
		}
		for (auto &c : candidates)
			if (kindList.count(c))
				return c;
	}
	throw std::runtime_error("source type not available in this OBS");
}

// >>> whole scene incl. group children, one relay hop
static json treeItems(const std::string &sceneName, bool isGroup)
{
	json d = obsRequest(isGroup ? "GetGroupSceneItemList" : "GetSceneItemList", {{"sceneName", sceneName}});
	json out = json::array();
	for (auto &it : arr(d, "sceneItems")) {
		std::string kind = jstr(it, "inputKind");
		json item = {
			{"sceneItemId", field(it, "sceneItemId")},
			{"sourceName", field(it, "sourceName")},
			{"inputKind", kind.empty() ? json() : json(kind)},
			{"isGroup", jtruthy(it, "isGroup")},
			{"sceneItemIndex", field(it, "sceneItemIndex")},
			{"enabled", field(it, "sceneItemEnabled")},
			{"locked", jtruthy(it, "sceneItemLocked")},
			{"transform", field(it, "sceneItemTransform").is_object() ? field(it, "sceneItemTransform") : json::object()},
		};
		if (!kind.empty() && std::regex_search(kind, SETTINGS_KINDS)) {
			try {
				json s = obsRequest("GetInputSettings", {{"inputName", jstr(it, "sourceName")}});
				json settings = field(s, "inputSettings");
				item["settings"] = settings.is_object() ? settings : json::object();
			} catch (...) {
			}
		}
		if (jtruthy(it, "isGroup")) {
			try {
				item["children"] = treeItems(jstr(it, "sourceName"), true);
			} catch (...) {
				item["children"] = json::array();
			}
		}
		out.push_back(item);
	}
	return out;
}

static double num(const json &j, const char *key, double def)
{
	double v;
	return jnum(j, key, v) ? v : def;
}

// >>> semantic kind from the dashboard -> real obs input
static json createFromElement(const json &data)
{
	json s = field(data, "settings");
	if (!s.is_object())
		s = json::object();
	json box = field(data, "box");
	if (!box.is_object())
		box = json::object();
	double bw = num(box, "w", 100), bh = num(box, "h", 100);
	std::string kindIn = jstr(data, "kind");
	std::string inputKind;
	json inputSettings;

	if (kindIn == "text") {
		inputKind = pickKind(TEXT_KINDS);
		bool gdi = startsWith(inputKind, "text_gdiplus");
		json font = {{"face", jstr(s, "fontFace", gdi ? "Arial" : "Sans Serif")},
			     {"size", num(s, "fontSize", 32)},
			     {"style", "Regular"},
			     {"flags", jtruthy(s, "bold") ? 1 : 0}};
		if (gdi) {
			inputSettings = {
				{"text", jstr(s, "text")},
				{"font", font},
				{"color", field(s, "color")},
				{"opacity", num(s, "opacity", 100)},
				{"align", jstr(s, "align", "left")},
				{"valign", jstr(s, "valign", "top")},
				{"extents", true},
				{"extents_cx", (int)std::lround(bw)},
				{"extents_cy", (int)std::lround(bh)},
				{"extents_wrap", true},
				{"outline", jtruthy(s, "outline")},
				{"outline_color", field(s, "outlineColor")},
				{"outline_size", num(s, "outlineSize", 2)},
				{"bk_color", num(s, "bkColor", 0)},
				{"bk_opacity", num(s, "bkOpacity", 0)},
			};
		} else {
			inputSettings = {
				{"text", jstr(s, "text")},
				{"font", font},
				{"color1", field(s, "colorAbgr")},
				{"color2", field(s, "colorAbgr")},
				{"outline", jtruthy(s, "outline")},
				{"word_wrap", true},
				{"custom_width", (int)std::lround(bw)},
			};
		}
	} else if (kindIn == "image") {
		inputKind = "image_source";
		inputSettings = {{"file", downloadToMedia(jstr(s, "url"))}};
	} else if (kindIn == "media") {
		inputKind = "ffmpeg_source";
		inputSettings = {{"is_local_file", false},
				 {"input", jstr(s, "url")},
				 {"looping", true},
				 {"restart_on_activate", true},
				 {"close_when_inactive", false}};
	} else if (kindIn == "browser") {
		inputKind = "browser_source";
		inputSettings = {{"url", jstr(s, "url")}, {"width", (int)std::lround(bw)}, {"height", (int)std::lround(bh)}};
	} else if (kindIn == "color") {
		inputKind = pickKind(COLOR_KINDS);
		inputSettings = {{"color", field(s, "colorAbgr")},
				 {"width", (int)std::lround(bw)},
				 {"height", (int)std::lround(bh)}};
	} else {
		throw std::runtime_error("unsupported kind");
	}
	// >>> nulls break obs settings, drop them
	for (auto it = inputSettings.begin(); it != inputSettings.end();)
		it = it->is_null() ? inputSettings.erase(it) : std::next(it);

	// >>> name taken = try "name 2", "name 3"...
	std::string base = jstr(data, "inputName");
	std::string sceneName = jstr(data, "sceneName");
	std::string name = base;
	json created;
	for (int n = 2; n < 12 && created.is_null(); n++) {
		try {
			created = obsRequest("CreateInput", {{"sceneName", sceneName},
							     {"inputName", name},
							     {"inputKind", inputKind},
							     {"inputSettings", inputSettings},
							     {"sceneItemEnabled", true}});
		} catch (const std::exception &e) {
			if (lower(e.what()).find("exist") == std::string::npos)
				throw;
			name = base + " " + std::to_string(n);
		}
	}
	if (created.is_null())
		throw std::runtime_error("couldn't find a free source name");

	// >>> gdi text/browser/color size themselves, the rest fits the box
	bool selfSized = startsWith(inputKind, "text_gdiplus") || inputKind == "browser_source" ||
			 startsWith(inputKind, "color_source");
	json transform = {{"positionX", num(box, "px", 0)},
			  {"positionY", num(box, "py", 0)},
			  {"rotation", num(box, "rotation", 0)},
			  {"alignment", 5},
			  {"boundsType", selfSized ? "OBS_BOUNDS_NONE" : "OBS_BOUNDS_SCALE_INNER"},
			  {"boundsAlignment", 0},
			  {"boundsWidth", std::max(1.0, bw)},
			  {"boundsHeight", std::max(1.0, bh)},
			  {"scaleX", 1},
			  {"scaleY", 1}};
	json itemId = field(created, "sceneItemId");
	obsRequest("SetSceneItemTransform",
		   {{"sceneName", sceneName}, {"sceneItemId", itemId}, {"sceneItemTransform", transform}});
	double idx;
	if (jnum(data, "sceneItemIndex", idx))
		obsRequest("SetSceneItemIndex",
			   {{"sceneName", sceneName}, {"sceneItemId", itemId}, {"sceneItemIndex", (int)idx}});
	return {{"sceneItemId", itemId}, {"inputName", name}};
}

static json sceneItemList(const json &data)
{
	std::string sceneName = jstr(data, "sceneName");
	json d = obsRequest("GetSceneItemList", {{"sceneName", sceneName}});
	json out = json::array();
	for (auto &it : arr(d, "sceneItems")) {
		std::string kind = jstr(it, "inputKind");
		std::string src = jstr(it, "sourceName");
		bool isAudio = !kind.empty() && AUDIO_KINDS.count(kind);
		json base = {{"sceneItemId", field(it, "sceneItemId")},
			     {"sourceName", src},
			     {"sceneItemEnabled", field(it, "sceneItemEnabled")},
			     {"inputKind", kind.empty() ? json() : json(kind)},
			     {"isAudioSource", isAudio},
			     {"sceneItemIndex", field(it, "sceneItemIndex")}};
		try {
			json t = obsRequest("GetSceneItemTransform",
					    {{"sceneName", sceneName}, {"sceneItemId", field(it, "sceneItemId")}});
			json tf = field(t, "sceneItemTransform");
			for (const char *k : {"positionX", "positionY", "scaleX", "scaleY", "rotation", "width", "height",
					      "sourceWidth", "sourceHeight", "alignment"})
				base[k] = field(tf, k);
		} catch (...) {
			// >>> groups skip transform fields
		}
		// >>> canvas iframes the real page instead of an empty box
		if (kind == "browser_source") {
			try {
				json s = obsRequest("GetInputSettings", {{"inputName", src}});
				base["url"] = field(field(s, "inputSettings"), "url");
			} catch (...) {
			}
		}
		if (isAudio) {
			try {
				json m = obsRequest("GetInputMute", {{"inputName", src}});
				json v = obsRequest("GetInputVolume", {{"inputName", src}});
				base["muted"] = jtruthy(m, "inputMuted");
				base["volumePercent"] = (int)std::lround(num(v, "inputVolumeMul", 0) * 100);
			} catch (...) {
			}
		}
		out.push_back(base);
	}
	return {{"sceneItems", out}};
}

// >>> relay's request names don't always match obs's exactly
json handleRelayRequest(const std::string &type, const json &dataIn)
{
	json data = dataIn.is_object() ? dataIn : json::object();
	std::string sceneName = jstr(data, "sceneName");
	std::string inputName = jstr(data, "inputName");

	if (type == "GetSceneList") {
		json d = obsRequest("GetSceneList");
		return {{"scenes", arr(d, "scenes")}, {"currentProgramSceneName", field(d, "currentProgramSceneName")}};
	}
	if (type == "SetCurrentProgramScene") {
		obsRequest(type, {{"sceneName", sceneName}});
		return json::object();
	}
	if (type == "GetSceneItemList")
		return sceneItemList(data);
	if (type == "GetSceneTree")
		return {{"items", treeItems(sceneName, false)}};
	if (type == "SetSceneItemLocked") {
		obsRequest(type, {{"sceneName", sceneName},
				  {"sceneItemId", field(data, "sceneItemId")},
				  {"sceneItemLocked", jtruthy(data, "locked")}});
		return json::object();
	}
	if (type == "RemoveSceneItem") {
		// >>> only this scene's item, input stays if used elsewhere
		obsRequest(type, {{"sceneName", sceneName}, {"sceneItemId", field(data, "sceneItemId")}});
		return json::object();
	}
	if (type == "SetInputName") {
		obsRequest(type, {{"inputName", inputName}, {"newInputName", jstr(data, "newInputName")}});
		return json::object();
	}
	if (type == "SetSourceSettings") {
		// >>> text + color only, never paths/urls from the dashboard
		json s = obsRequest("GetInputSettings", {{"inputName", inputName}});
		std::string kind = jstr(s, "inputKind");
		std::vector<std::string> allowed;
		if (startsWith(kind, "text_gdiplus"))
			allowed = {"text", "color"};
		else if (startsWith(kind, "text_ft2"))
			allowed = {"text", "color1", "color2"};
		else if (startsWith(kind, "color_source"))
			allowed = {"color"};
		json in = field(data, "settings");
		json patch = json::object();
		for (auto &k : allowed) {
			json v = field(in, k.c_str());
			if (!v.is_null())
				patch[k] = v;
		}
		if (patch.empty())
			throw std::runtime_error("nothing to change");
		obsRequest("SetInputSettings", {{"inputName", inputName}, {"inputSettings", patch}, {"overlay", true}});
		return json::object();
	}
	if (type == "CreateFromElement")
		return createFromElement(data);
	if (type == "ReadInputFile") {
		// >>> only the source's own file, never a free path
		json s = obsRequest("GetInputSettings", {{"inputName", inputName}});
		if (jstr(s, "inputKind") != "image_source")
			throw std::runtime_error("not an image source");
		std::string file = jstr(field(s, "inputSettings"), "file");
		if (file.empty() || !std::regex_search(file, IMAGE_EXT))
			throw std::runtime_error("no image file");
		fs::path p = fs::u8path(file);
		std::error_code ec;
		auto size = fs::file_size(p, ec);
		if (ec)
			throw std::runtime_error("file not found");
		if (size > MAX_FILE_BYTES)
			throw std::runtime_error("file too big");
		std::ifstream f(p, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string ext = lower(u8(p.extension()));
		if (!ext.empty())
			ext = ext.substr(1);
		std::string mime = ext == "svg" ? "image/svg+xml" : ext == "jpg" ? "image/jpeg" : "image/" + ext;
		return {{"dataUrl", "data:" + mime + ";base64," + base64(bytes)}, {"fileName", u8(p.filename())}};
	}
	if (type == "GetVideoSettings") {
		json d = obsRequest(type);
		return {{"baseWidth", field(d, "baseWidth")}, {"baseHeight", field(d, "baseHeight")}};
	}
	if (type == "SetSceneItemTransform") {
		json t = field(data, "transform");
		obsRequest(type, {{"sceneName", sceneName},
				  {"sceneItemId", field(data, "sceneItemId")},
				  {"sceneItemTransform", t.is_object() ? t : json::object()}});
		return json::object();
	}
	if (type == "SetSceneItemEnabled") {
		obsRequest(type, {{"sceneName", sceneName},
				  {"sceneItemId", field(data, "sceneItemId")},
				  {"sceneItemEnabled", jtruthy(data, "enabled")}});
		return json::object();
	}
	if (type == "SetSceneItemIndex") {
		obsRequest(type, {{"sceneName", sceneName},
				  {"sceneItemId", field(data, "sceneItemId")},
				  {"sceneItemIndex", field(data, "sceneItemIndex")}});
		return json::object();
	}
	if (type == "CreateInput") {
		// >>> browser source only for now
		json d = obsRequest(type, {{"sceneName", sceneName},
					   {"inputName", inputName},
					   {"inputKind", "browser_source"},
					   {"inputSettings",
					    {{"url", jstr(data, "url")},
					     {"width", num(data, "width", 1920)},
					     {"height", num(data, "height", 1080)}}},
					   {"sceneItemEnabled", true}});
		return {{"sceneItemId", field(d, "sceneItemId")}};
	}
	if (type == "CreateSceneItem") {
		// >>> existing input into another scene, CreateInput can't do that
		json d = obsRequest(type, {{"sceneName", sceneName},
					   {"sourceName", jstr(data, "sourceName")},
					   {"sceneItemEnabled", true}});
		return {{"sceneItemId", field(d, "sceneItemId")}};
	}
	if (type == "GetInputMute") {
		json d = obsRequest(type, {{"inputName", inputName}});
		return {{"muted", jtruthy(d, "inputMuted")}};
	}
	if (type == "SetInputMute") {
		obsRequest(type, {{"inputName", inputName}, {"inputMuted", jtruthy(data, "muted")}});
		return json::object();
	}
	if (type == "ToggleInputMute") {
		json d = obsRequest(type, {{"inputName", inputName}});
		return {{"muted", jtruthy(d, "inputMuted")}};
	}
	if (type == "GetInputVolume") {
		json d = obsRequest(type, {{"inputName", inputName}});
		return {{"percent", (int)std::lround(num(d, "inputVolumeMul", 0) * 100)}};
	}
	if (type == "SetInputVolume") {
		double pct = std::max(0.0, std::min(100.0, num(data, "percent", 0)));
		obsRequest(type, {{"inputName", inputName}, {"inputVolumeMul", pct / 100}});
		return json::object();
	}
	if (type == "GetSourceScreenshot") {
		// >>> jpeg under 60 quality gets muddy, cap it there
		int width = (int)std::max(8.0, std::min(4096.0, std::round(num(data, "width", 1000))));
		int quality = (int)std::max(60.0, std::min(100.0, std::round(num(data, "quality", 75))));
		auto t0 = std::chrono::steady_clock::now();
		json d = obsRequest(type, {{"sourceName", jstr(data, "sourceName")},
					   {"imageFormat", jstr(data, "format") == "png" ? "png" : "jpg"},
					   {"imageWidth", width},
					   {"imageCompressionQuality", quality}});
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		return {{"imageData", field(d, "imageData")}, {"cpuMs", ms}};
	}
	if (type == "GetVideoMixProjectorState")
		return probeProjector();
	if (type == "OpenVideoMixProjector") {
		json state = probeProjector();
		if (jtruthy(state, "open"))
			return state;
		obsRequest(type, {{"videoMixType", jstr(data, "videoMixType", "OBS_WEBSOCKET_VIDEO_MIX_TYPE_MULTIVIEW")},
				  {"monitorIndex", -1}});
		return probeProjector();
	}
	if (type == "OpenSourceProjector") {
		obsRequest(type, {{"sourceName", jstr(data, "sourceName")}, {"monitorIndex", -1}, {"projectorGeometry", ""}});
		return json::object();
	}
	if (type == "GetStats")
		return obsRequest(type);
	if (type == "RemoveInput") {
		obsRequest(type, {{"inputName", inputName}});
		return json::object();
	}
	if (type == "SetInputUrl") {
		obsRequest("SetInputSettings",
			   {{"inputName", inputName}, {"inputSettings", {{"url", jstr(data, "url")}}}, {"overlay", true}});
		return json::object();
	}
	if (type == "SetupAudioSource") {
		// >>> song request player: sound through obs, only on picked tracks
		json s = obsRequest("GetInputSettings", {{"inputName", inputName}});
		if (jstr(s, "inputKind") != "browser_source")
			throw std::runtime_error("not a browser source");
		obsRequest("SetInputSettings",
			   {{"inputName", inputName}, {"inputSettings", {{"reroute_audio", true}}}, {"overlay", true}});
		json mon = field(data, "monitor");
		bool off = mon.is_boolean() && !mon.get<bool>();
		obsRequest("SetInputAudioMonitorType",
			   {{"inputName", inputName},
			    {"monitorType", off ? "OBS_MONITORING_TYPE_NONE" : "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT"}});
		json tracks = arr(data, "tracks");
		if (!tracks.empty()) {
			json t = json::object();
			for (int i = 1; i <= 6; i++)
				t[std::to_string(i)] = std::find(tracks.begin(), tracks.end(), json(i)) != tracks.end();
			obsRequest("SetInputAudioTracks", {{"inputName", inputName}, {"inputAudioTracks", t}});
		}
		return json::object();
	}
	if (type == "GetInputAudio") {
		json m = obsRequest("GetInputAudioMonitorType", {{"inputName", inputName}});
		json t = obsRequest("GetInputAudioTracks", {{"inputName", inputName}});
		json tracks = json::array();
		json at = field(t, "inputAudioTracks");
		if (at.is_object())
			for (auto &[k, on] : at.items())
				if (on.is_boolean() && on.get<bool>())
					tracks.push_back(std::atoi(k.c_str()));
		return {{"monitor", jstr(m, "monitorType") != "OBS_MONITORING_TYPE_NONE"}, {"tracks", tracks}};
	}
	if (type == "RefreshBrowserSource") {
		obsRequest("PressInputPropertiesButton", {{"inputName", inputName}, {"propertyName", "refreshnocache"}});
		return json::object();
	}
	throw std::runtime_error("unknown request type \"" + type + "\"");
}

} // namespace shy
