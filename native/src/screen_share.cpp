#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace haven {

constexpr int kProtocolVersion = 6;

constexpr int kPickerTimeoutSeconds = 60;
constexpr int kFirstFrameTimeoutSeconds = 10;
constexpr int kNegotiationTimeoutSeconds = 10;
constexpr int kIceDisconnectedGraceSeconds = 3;
constexpr int kReconnectBudget = 3;
constexpr int kMaxConcurrentPeers = 16;
constexpr int kMaxPeerGenerations = 64;
constexpr int kMaxIceCandidatesPerPeer = 256;
constexpr int kMaxSdpLine = 4096;
constexpr int kMaxProtocolLineSize = 8 * 1024;
constexpr int kPcmRingSizeMs = 200;

enum class State {
  kIdle,
  kPicker,
  kSource,
  kNegotiating,
  kConnected,
  kStopping,
  kStopped,
  kFailed,
};

const char* state_name(State s) {
  switch (s) {
    case State::kIdle: return "Idle";
    case State::kPicker: return "Picker";
    case State::kSource: return "Source";
    case State::kNegotiating: return "Negotiating";
    case State::kConnected: return "Connected";
    case State::kStopping: return "Stopping";
    case State::kStopped: return "Stopped";
    case State::kFailed: return "Failed";
  }
  return "Unknown";
}

enum class SourceKind {
  kAuto,
  kLinuxPortal,
  kLinuxX11,
  kWindowsDxgi,
  kWindowsWgc,
  kTest,
};

const char* source_kind_name(SourceKind kind) {
  switch (kind) {
    case SourceKind::kAuto: return "auto";
    case SourceKind::kLinuxPortal: return "linux-portal";
    case SourceKind::kLinuxX11: return "linux-x11";
    case SourceKind::kWindowsDxgi: return "windows-dxgi";
    case SourceKind::kWindowsWgc: return "windows-wgc";
    case SourceKind::kTest: return "test";
  }
  return "unknown";
}

SourceKind parse_source_kind(const std::string& s) {
  if (s == "auto") return SourceKind::kAuto;
  if (s == "linux-portal") return SourceKind::kLinuxPortal;
  if (s == "linux-x11") return SourceKind::kLinuxX11;
  if (s == "windows-dxgi") return SourceKind::kWindowsDxgi;
  if (s == "windows-wgc") return SourceKind::kWindowsWgc;
  if (s == "test") return SourceKind::kTest;
  return SourceKind::kAuto;
}

enum class Codec {
  kH264,
  kH265,
  kVp8,
  kVp9,
};

const char* codec_name(Codec c) {
  switch (c) {
    case Codec::kH264: return "H264";
    case Codec::kH265: return "H265";
    case Codec::kVp8: return "VP8";
    case Codec::kVp9: return "VP9";
  }
  return "unknown";
}

struct CapturePlan {
  SourceKind sourceKind = SourceKind::kAuto;
  std::string sourceHandle;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int fps = 60;
  int bitrateKbps = 6000;
  int scalePercent = 100;
  Codec codec = Codec::kH264;
  bool audioSystem = false;
  bool audioApp = false;
};

struct Encoder {
  Codec codec;
  const char* factoryName;
  bool hardware;
};

bool factory_exists(const char* name) {
  GstElementFactory* factory = gst_element_factory_find(name);
  if (!factory) return false;
  gst_object_unref(factory);
  return true;
}

Encoder first_available_encoder(Codec codec) {
  struct Entry { const char* factory; bool hardware; };
  switch (codec) {
    case Codec::kH264:
      for (auto e : (Entry[]){{"nvh264enc", true}, {"vah264enc", true}, {"mfh264enc", true},
                               {"x264enc", false}, {"openh264enc", false}}) {
        if (factory_exists(e.factory)) return {codec, e.factory, e.hardware};
      }
      break;
    case Codec::kH265:
      for (auto e : (Entry[]){{"nvh265enc", true}, {"vah265enc", true}, {"mfh265enc", true},
                               {"x265enc", false}}) {
        if (factory_exists(e.factory)) return {codec, e.factory, e.hardware};
      }
      break;
    case Codec::kVp8:
      if (factory_exists("vp8enc")) return {codec, "vp8enc", false};
      break;
    case Codec::kVp9:
      for (auto e : (Entry[]){{"vp9enc", false}}) {
        if (factory_exists(e.factory)) return {codec, e.factory, e.hardware};
      }
      break;
  }
  return {codec, nullptr, false};
}

SourceKind resolve_source_kind(SourceKind requested) {
  if (requested != SourceKind::kAuto) return requested;
  const char* session_type = std::getenv("XDG_SESSION_TYPE");
  if (session_type && std::strcmp(session_type, "wayland") == 0 &&
      factory_exists("pipewiresrc")) {
    return SourceKind::kLinuxPortal;
  }
  if (std::getenv("DISPLAY") && factory_exists("ximagesrc")) {
    return SourceKind::kLinuxX11;
  }
#ifdef G_OS_WIN32
  if (factory_exists("d3d11screencapturesrc")) return SourceKind::kWindowsDxgi;
#endif
  return SourceKind::kLinuxX11;
}

class Emitter {
 public:
  void Emit(const std::string& event, const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << event;
    for (const auto& f : fields) {
      std::cout << '\t';
      for (char c : f) {
        switch (c) {
          case '\\': std::cout << "\\\\"; break;
          case '\t': std::cout << "\\t"; break;
          case '\n': std::cout << "\\n"; break;
          case '\r': std::cout << "\\r"; break;
          default: std::cout << c;
        }
      }
    }
    std::cout << '\n';
    std::cout.flush();
  }

 private:
  std::mutex mutex_;
};

struct Command {
  std::string name;
  std::vector<std::string> fields;
};

class CommandQueue {
 public:
  void Push(Command cmd) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.size() >= kMaxPendingCommands) {
        queue_.pop_front();
      }
      queue_.push_back(std::move(cmd));
    }
    cv_.notify_one();
  }

  std::optional<Command> PopFor(int state_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(state_ms),
        [this] { return !queue_.empty() || closed_; })) {
      return std::nullopt;
    }
    if (queue_.empty()) return std::nullopt;
    Command cmd = std::move(queue_.front());
    queue_.pop_front();
    return cmd;
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

 private:
  static constexpr size_t kMaxPendingCommands = 64;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Command> queue_;
  bool closed_ = false;
};

Command parse_command(const std::string& line) {
  if (line.empty() || line.size() > kMaxProtocolLineSize) return {};
  std::vector<std::string> fields;
  std::string current;
  for (char c : line) {
    if (c == '\t') { fields.push_back(std::move(current)); current.clear(); }
    else if (c == '\r') continue;
    else current.push_back(c);
  }
  fields.push_back(std::move(current));
  Command cmd;
  if (fields.empty()) return cmd;
  cmd.name = std::move(fields.front());
  fields.erase(fields.begin());
  cmd.fields = std::move(fields);
  return cmd;
}

class Session;

int run_probe(Emitter& emitter) {
  const bool x11 = factory_exists("ximagesrc");
  const bool pipewire = factory_exists("pipewiresrc");
  const bool transport = factory_exists("webrtcbin") && factory_exists("nicesrc") &&
      factory_exists("nicesink") && factory_exists("dtlsenc") &&
      factory_exists("srtpenc");
  const bool audio = factory_exists("appsrc") && factory_exists("opusenc") &&
      factory_exists("rtpopuspay");
  const auto h264 = first_available_encoder(Codec::kH264);
  const auto h265 = first_available_encoder(Codec::kH265);
  const bool encoder = h264.factoryName != nullptr || h265.factoryName != nullptr;
  const bool capture = x11 || pipewire;
  const bool supported = capture && encoder && transport;

  std::ostringstream out;
  out << "{\"protocolVersion\":" << kProtocolVersion
      << ",\"supported\":" << (supported ? "true" : "false")
      << ",\"captureBackends\":[";
  bool first = true;
  if (x11) { if (!first) out << ','; out << "\"x11\""; first = false; }
  if (pipewire) { if (!first) out << ','; out << "\"pipewire-portal\""; first = false; }
#ifdef G_OS_WIN32
  if (first) { out << "\"dxgi\""; first = false; }
#endif
  out << "],\"encoders\":[";
  first = true;
  auto emit = [&](const Encoder& e) {
    if (!e.factoryName) return;
    if (!first) out << ',';
    first = false;
    out << "{\"codec\":\"" << codec_name(e.codec) << "\",\"factory\":\""
        << e.factoryName << "\",\"hardware\":" << (e.hardware ? "true" : "false") << '}';
  };
  emit(h264);
  emit(h265);
  out << "],\"audio\":{\"supported\":" << (audio ? "true" : "false")
      << ",\"codec\":\"OPUS\",\"sampleRate\":48000,\"channels\":1}"
      << ",\"components\":{\"capture\":" << (capture ? "true" : "false")
      << ",\"encoder\":" << (encoder ? "true" : "false")
      << ",\"audio\":" << (audio ? "true" : "false")
      << ",\"transport\":" << (transport ? "true" : "false") << "}}";
  if (!supported) {
    out << ",\"reason\":\"no-backend\"";
  }
  std::cout << out.str() << std::endl;
  return supported ? 0 : 2;
}

struct Peer {
  std::string id;
  GstElement* webrtcbin = nullptr;
  GstElement* queue = nullptr;
  int generation = 0;
  bool offerPending = false;
  std::vector<std::string> pendingIce;
  bool connected = false;
};

struct OfferContext {
  Session* session;
  std::string peer_id;
};

class Session {
 public:
  Session(Emitter& emitter, CommandQueue& queue)
      : emitter_(emitter), queue_(queue) {}

  ~Session() {
    Stop("destructor");
  }

  void Start(const CapturePlan& plan) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kIdle && state_ != State::kStopped) {
      emitter_.Emit("COMMAND_RESULT",
        {"start", "0", "false", "session-already-active"});
      return;
    }
    plan_ = plan;
    const bool needsPicker = plan.sourceKind == SourceKind::kLinuxPortal
        || plan.sourceKind == SourceKind::kWindowsDxgi
        || plan.sourceKind == SourceKind::kWindowsWgc;
    if (needsPicker) {
      state_ = State::kPicker;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("PICKER_NEEDED", {source_kind_name(plan.sourceKind)});
      pickerDeadline_ = std::chrono::steady_clock::now() +
          std::chrono::seconds(kPickerTimeoutSeconds);
    } else {
      StartSource();
    }
  }

  void HandlePickerResult(const std::string& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kPicker) return;
    plan_.sourceHandle = handle;
    emitter_.Emit("PICKER_RESULT", {source_kind_name(plan_.sourceKind), handle,
      std::to_string(plan_.sourceWidth), std::to_string(plan_.sourceHeight)});
    StartSource();
  }

  void AddPeer(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (peers_.size() >= static_cast<size_t>(kMaxConcurrentPeers)) {
      emitter_.Emit("COMMAND_RESULT", {id, "peer_add", "0", "peer-limit"});
      return;
    }
    Peer peer;
    peer.id = id;
    peer.webrtcbin = gst_element_factory_make("webrtcbin", id.c_str());
    if (!peer.webrtcbin) {
      emitter_.Emit("COMMAND_RESULT", {id, "peer_add", "0", "factory-missing"});
      return;
    }
    g_object_set(peer.webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
    gst_bin_add(GST_BIN(pipeline_), peer.webrtcbin);
    if (!LinkPeerToMuxer(peer)) {
      emitter_.Emit("COMMAND_RESULT", {id, "peer_add", "0", "link-failed"});
      gst_bin_remove(GST_BIN(pipeline_), peer.webrtcbin);
      return;
    }
    g_signal_connect(peer.webrtcbin, "on-negotiation-needed", G_CALLBACK(OnNegotiationNeeded), this);
    g_signal_connect(peer.webrtcbin, "on-ice-candidate", G_CALLBACK(OnIceCandidate), this);
    peers_.emplace(id, std::move(peer));
    g_signal_emit_by_name(peer.webrtcbin, "on-negotiation-needed", nullptr);
    emitter_.Emit("COMMAND_RESULT", {id, "peer_add", "1", ""});
  }

  void RemovePeer(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end()) {
      emitter_.Emit("COMMAND_RESULT", {id, "peer_remove", "0", "unknown-peer"});
      return;
    }
    GstElement* queue = it->second.queue;
    if (queue && pipeline_) {
      gst_element_set_state(queue, GST_STATE_NULL);
      gst_bin_remove(GST_BIN(pipeline_), queue);
      it->second.queue = nullptr;
    }
    if (it->second.webrtcbin) {
      gst_element_set_state(it->second.webrtcbin, GST_STATE_NULL);
      gst_bin_remove(GST_BIN(pipeline_), it->second.webrtcbin);
      it->second.webrtcbin = nullptr;
    }
    peers_.erase(it);
    emitter_.Emit("COMMAND_RESULT", {id, "peer_remove", "1", ""});
  }

  void HandleRemoteDesc(const std::string& peer_id, const std::string& type,
                        const std::string& sdp) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) return;
    Peer& peer = it->second;
    GstSDPMessage* sdp_message = nullptr;
    const std::string& sdp_to_use = sdp.size() > kMaxSdpLine ? sdp.substr(0, kMaxSdpLine) : sdp;
    if (gst_sdp_message_new_from_text(sdp_to_use.c_str(), &sdp_message) != GST_SDP_OK) {
      emitter_.Emit("ERROR", {"sdp-parse", "1", "Invalid remote SDP"});
      return;
    }
    GstWebRTCSessionDescription* desc = gst_webrtc_session_description_new(
        type == "answer" ? GST_WEBRTC_SDP_TYPE_ANSWER : GST_WEBRTC_SDP_TYPE_OFFER,
        sdp_message);
    g_signal_emit_by_name(peer.webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);
    emitter_.Emit("COMMAND_RESULT", {peer_id, "remote_desc", "1", ""});
  }

  void HandleRemoteIce(const std::string& peer_id, const std::string& candidate,
                       int sdp_m_line_index, const std::string& sdp_mid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) return;
    Peer& peer = it->second;
    g_signal_emit_by_name(peer.webrtcbin, "add-ice-candidate",
        sdp_m_line_index, sdp_mid.c_str(), candidate.c_str());
    emitter_.Emit("COMMAND_RESULT", {peer_id, "remote_ice", "1", ""});
  }

  void SetBitrate(int kbps) {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.bitrateKbps = std::clamp(kbps, 500, 50000);
    ApplyBitrate();
  }

  void SetFps(int fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.fps = (fps == 30 || fps == 60) ? fps : plan_.fps;
  }

  void SetScale(int percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.scalePercent = std::clamp(percent, 50, 100);
  }

  void Stop(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kStopped || state_ == State::kFailed) {
      emitter_.Emit("STOPPED", {reason});
      return;
    }
    state_ = State::kStopping;
    emitter_.Emit("STATE", {state_name(state_)});
    for (auto& [id, peer] : peers_) {
      if (peer.webrtcbin) {
        gst_element_set_state(peer.webrtcbin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline_), peer.webrtcbin);
        peer.webrtcbin = nullptr;
      }
    }
    peers_.clear();
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    state_ = State::kStopped;
    emitter_.Emit("STOPPED", {reason});
    emitter_.Emit("STATE", {state_name(state_)});
  }

  State GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  void CheckDeadlines() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (state_ == State::kPicker && now >= pickerDeadline_) {
      state_ = State::kStopped;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("STOPPED", {"picker-timeout"});
      return;
    }
    if (state_ == State::kSource && now >= firstFrameDeadline_) {
      state_ = State::kFailed;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("ERROR", {"first-frame-timeout", "1",
                              "capture pipeline did not produce a frame in time"});
    }
  }

 private:
  void StartSource() {
    state_ = State::kSource;
    emitter_.Emit("STATE", {state_name(state_)});
    if (!BuildPipeline()) {
      state_ = State::kFailed;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("ERROR", {"pipeline-build", "1", "Failed to build capture pipeline"});
      return;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
      state_ = State::kFailed;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("ERROR", {"pipeline-state", "1", "Failed to set pipeline to READY"});
      return;
    }
    state_ = State::kNegotiating;
    emitter_.Emit("STATE", {state_name(state_)});
    if (gst_element_set_state(pipeline_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      state_ = State::kFailed;
      emitter_.Emit("STATE", {state_name(state_)});
      emitter_.Emit("ERROR", {"pipeline-state", "1", "Failed to set pipeline to PAUSED"});
      return;
    }
    firstFrameDeadline_ = std::chrono::steady_clock::now() +
        std::chrono::seconds(kFirstFrameTimeoutSeconds);
  }

  bool BuildPipeline() {
    pipeline_ = gst_pipeline_new("haven-screen-share");
    GstElement* source = nullptr;
    switch (plan_.sourceKind) {
      case SourceKind::kLinuxPortal: {
        source = gst_element_factory_make("pipewiresrc", "src");
        if (!source) return false;
        g_object_set(source, "path", plan_.sourceHandle.empty() ? "0" : plan_.sourceHandle.c_str(), nullptr);
        g_object_set(source, "do-timestamp", TRUE, nullptr);
        break;
      }
      case SourceKind::kLinuxX11:
        source = gst_element_factory_make("ximagesrc", "src");
        if (!source) return false;
        g_object_set(source, "use-damage", FALSE, nullptr);
        g_object_set(source, "show-pointer", TRUE, nullptr);
        break;
      case SourceKind::kWindowsDxgi:
      case SourceKind::kWindowsWgc:
        source = gst_element_factory_make("d3d11screencapturesrc", "src");
        if (!source) return false;
        break;
      case SourceKind::kTest:
        source = gst_element_factory_make("videotestsrc", "src");
        if (!source) return false;
        g_object_set(source, "is-live", TRUE, nullptr);
        break;
      default:
        return false;
    }
    gst_bin_add(GST_BIN(pipeline_), source);

    GstElement* convert = gst_element_factory_make("videoconvertscale", "convert");
    if (!convert) return false;
    gst_bin_add(GST_BIN(pipeline_), convert);

    const char* hw_format = "I420";
    int out_w = plan_.sourceWidth > 0 ? plan_.sourceWidth : 1920;
    int out_h = plan_.sourceHeight > 0 ? plan_.sourceHeight : 1080;
    if (plan_.scalePercent < 100) {
      out_w = out_w * plan_.scalePercent / 100;
      out_h = out_h * plan_.scalePercent / 100;
    }
    char caps_str[256];
    std::snprintf(caps_str, sizeof(caps_str),
        "video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1",
        hw_format, out_w, out_h, plan_.fps);
    GstCaps* out_caps = gst_caps_from_string(caps_str);
    GstElement* capsfilter = gst_element_factory_make("capsfilter", "caps");
    if (!capsfilter) { gst_caps_unref(out_caps); return false; }
    g_object_set(capsfilter, "caps", out_caps, nullptr);
    gst_caps_unref(out_caps);
    gst_bin_add(GST_BIN(pipeline_), capsfilter);

    Encoder enc = first_available_encoder(plan_.codec);
    if (!enc.factoryName) {
      emitter_.Emit("ERROR", {"encoder", "1", "No hardware or software encoder available"});
      return false;
    }
    GstElement* encoder = gst_element_factory_make(enc.factoryName, "encoder");
    if (!encoder) return false;
    encoder_ = encoder;
    gst_bin_add(GST_BIN(pipeline_), encoder);

    bool is_va_hw = (std::strcmp(enc.factoryName, "vah264enc") == 0
                  || std::strcmp(enc.factoryName, "vah265enc") == 0);
    GstElement* va_postproc = nullptr;
    if (is_va_hw) {
      va_postproc = gst_element_factory_make("vaapipostproc", "vaproc");
      if (!va_postproc) {
        gst_object_unref(encoder);
        encoder_ = nullptr;
        encoder = gst_element_factory_make("x264enc", "encoder");
        if (!encoder) return false;
        encoder_ = encoder;
        is_va_hw = false;
      } else {
        gst_bin_add(GST_BIN(pipeline_), va_postproc);
      }
    }

    GstElement* queue = gst_element_factory_make("queue", "vqueue");
    g_object_set(queue, "max-size-buffers", 4, "leaky", 2, nullptr);
    gst_bin_add(GST_BIN(pipeline_), queue);
    ApplyBitrate();

    GstElement* parser = gst_element_factory_make(
        plan_.codec == Codec::kH265 ? "h265parse" : "h264parse", "parser");
    if (!parser) return false;
    gst_bin_add(GST_BIN(pipeline_), parser);

    GstElement* payloader = gst_element_factory_make(
        plan_.codec == Codec::kH265 ? "rtph265pay" : "rtph264pay", "payloader");
    if (!payloader) return false;
    gst_bin_add(GST_BIN(pipeline_), payloader);
    g_object_set(payloader, "config-interval", 1, nullptr);

    GstElement* prev = source;
    auto link = [&](GstElement* next) {
      if (!prev || !next) return false;
      if (!gst_element_link(prev, next)) return false;
      prev = next;
      return true;
    };
    if (!link(convert)) return false;
    if (!link(capsfilter)) return false;
    if (va_postproc && !link(va_postproc)) return false;
    if (!link(encoder)) return false;
    if (!link(queue)) return false;
    if (!link(parser)) return false;
    if (!link(payloader)) return false;

    if (plan_.audioSystem) {
      GstElement* audioSrc = gst_element_factory_make("pipewiresrc", "audio-src");
      if (audioSrc) {
        g_object_set(audioSrc, "stream-type", "raw", nullptr);
        g_object_set(audioSrc, "client-name", "Haven", nullptr);
        gst_bin_add(GST_BIN(pipeline_), audioSrc);
        GstElement* aconv = gst_element_factory_make("audioconvert", nullptr);
        GstElement* aresamp = gst_element_factory_make("audioresample", nullptr);
        GstElement* opus = gst_element_factory_make("opusenc", nullptr);
        GstElement* rtppay = gst_element_factory_make("rtpopuspay", nullptr);
        if (aconv && aresamp && opus && rtppay) {
          gst_bin_add_many(GST_BIN(pipeline_), aconv, aresamp, opus, rtppay, nullptr);
          gst_element_link_many(audioSrc, aconv, aresamp, opus, rtppay, nullptr);
        }
      }
    }

    GstElement* tee = gst_element_factory_make("tee", "vtee");
    if (!tee) return false;
    gst_bin_add(GST_BIN(pipeline_), tee);
    if (!gst_element_link(payloader, tee)) return false;
    video_tee_ = tee;

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    gst_bus_add_watch(bus, OnBusMessage, this);
    gst_object_unref(bus);

    return true;
  }

  GstCaps* BuildCaps() {
    const char* format = "BGRA";
    int w = plan_.sourceWidth > 0 ? plan_.sourceWidth : 1920;
    int h = plan_.sourceHeight > 0 ? plan_.sourceHeight : 1080;
    if (plan_.scalePercent < 100) {
      w = w * plan_.scalePercent / 100;
      h = h * plan_.scalePercent / 100;
    }
    return gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, format,
        "width", G_TYPE_INT, w,
        "height", G_TYPE_INT, h,
        "framerate", GST_TYPE_FRACTION, plan_.fps, 1,
        nullptr);
  }

  void ApplyBitrate() {
    if (!encoder_) return;
    GstElementFactory* factory = gst_element_get_factory(encoder_);
    if (!factory) return;
    const char* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (!factory_name) return;
    std::string name = factory_name;
    int kbps = plan_.bitrateKbps;
    if (name == "x264enc") {
      g_object_set(encoder_, "bitrate", kbps, "pass", 4, nullptr);
    } else if (name == "openh264enc") {
      g_object_set(encoder_, "bitrate", kbps, nullptr);
    } else if (name == "x265enc") {
      g_object_set(encoder_, "bitrate", kbps, nullptr);
    } else if (name == "vah264enc" || name == "vah265enc") {
      g_object_set(encoder_, "bitrate", kbps, nullptr);
    } else if (name == "nvh264enc" || name == "nvh265enc") {
      g_object_set(encoder_, "bitrate", kbps, nullptr);
    } else if (name == "mfh264enc" || name == "mfh265enc") {
      g_object_set(encoder_, "target-bitrate", kbps, nullptr);
    }
  }

  bool LinkPeerToMuxer(Peer& peer) {
    if (!pipeline_) return false;
    GstElement* queue = gst_element_factory_make("queue", ("queue_" + peer.id).c_str());
    if (!queue) return false;
    gst_bin_add(GST_BIN(pipeline_), queue);

    GstPad* teeSrc = gst_element_request_pad_simple(video_tee_, "src_%u");
    if (!teeSrc) { gst_object_unref(queue); return false; }
    GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(teeSrc, queueSink) != GST_PAD_LINK_OK) {
      gst_object_unref(teeSrc);
      gst_object_unref(queueSink);
      gst_object_unref(queue);
      return false;
    }
    gst_object_unref(teeSrc);
    gst_object_unref(queueSink);

    peer.queue = queue;
    g_signal_connect(queue, "pad-added", G_CALLBACK(OnQueuePadAdded), this);
    return true;
  }

  static void OnNegotiationNeeded(GstElement* webrtcbin, gpointer user_data) {
    Session* self = static_cast<Session*>(user_data);
    std::string peer_id = gst_element_get_name(webrtcbin);
    auto* ctx = new OfferContext{self, peer_id};
    GstPromise* promise = gst_promise_new_with_change_func(
        [](GstPromise* promise, gpointer data) {
          auto* ctx = static_cast<OfferContext*>(data);
          Session* session = ctx->session;
          const GstStructure* reply = gst_promise_get_reply(promise);
          if (reply) {
            GstWebRTCSessionDescription* offer = nullptr;
            gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                            &offer, nullptr);
            if (offer) {
              gchar* sdp_text = gst_sdp_message_as_text(offer->sdp);
              std::string sdp = sdp_text ? sdp_text : "";
              g_free(sdp_text);
              gst_webrtc_session_description_free(offer);
              session->emitter_.Emit("OFFER", {ctx->peer_id, sdp});
            }
          }
          delete ctx;
        }, ctx, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
    gst_promise_unref(promise);
  }

  static void OnIceCandidate(GstElement* webrtcbin, guint m_line_index,
                             gchar* candidate, gpointer user_data) {
    Session* self = static_cast<Session*>(user_data);
    std::string peer_id = gst_element_get_name(webrtcbin);
    std::string cand = candidate ? candidate : "";
    self->emitter_.Emit("ICE", {peer_id, cand, "", std::to_string(m_line_index), ""});
  }

  static void OnQueuePadAdded(GstElement* queue, GstPad* srcPad, gpointer user_data) {
    Session* self = static_cast<Session*>(user_data);
    std::string queueName = gst_element_get_name(queue);
    std::string peerId = queueName.substr(6);
    std::lock_guard<std::mutex> lock(self->mutex_);
    auto it = self->peers_.find(peerId);
    if (it == self->peers_.end()) return;
    GstElement* webrtcbin = it->second.webrtcbin;
    if (!webrtcbin) return;
    GstPad* sinkPad = gst_element_get_static_pad(webrtcbin, "sink_%d");
    if (!sinkPad) {
      if (gst_pad_link(srcPad, gst_element_get_static_pad(webrtcbin, "sink")) != GST_PAD_LINK_OK) {
        g_warning("Failed to link queue to webrtcbin for peer %s", peerId.c_str());
      }
    } else {
      if (gst_pad_link(srcPad, sinkPad) != GST_PAD_LINK_OK) {
        g_warning("Failed to link queue to webrtcbin sink for peer %s", peerId.c_str());
      }
      gst_object_unref(sinkPad);
    }
  }

  static gboolean OnBusMessage(GstBus* bus, GstMessage* message, gpointer user_data) {
    Session* self = static_cast<Session*>(user_data);
    switch (GST_MESSAGE_TYPE(message)) {
      case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state, new_state, pending;
        gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline_)) {
          if (new_state == GST_STATE_PLAYING && old_state != GST_STATE_PLAYING) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->state_ == State::kNegotiating) {
              self->state_ = State::kConnected;
              self->emitter_.Emit("STATE", {state_name(self->state_)});
            }
          }
        }
        break;
      }
      case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &err, &debug);
        self->emitter_.Emit("ERROR", {"gstreamer", err ? "1" : "0", err ? err->message : ""});
        g_clear_error(&err);
        g_free(debug);
        break;
      }
      case GST_MESSAGE_EOS:
        self->emitter_.Emit("STATE", {"EOS"});
        break;
      default:
        break;
    }
    return TRUE;
  }

  Emitter& emitter_;
  CommandQueue& queue_;
  CapturePlan plan_;
  GstElement* pipeline_ = nullptr;
  GstElement* encoder_ = nullptr;
  GstElement* video_tee_ = nullptr;
  std::unordered_map<std::string, Peer> peers_;
  State state_ = State::kIdle;
  std::chrono::steady_clock::time_point pickerDeadline_;
  std::chrono::steady_clock::time_point firstFrameDeadline_;
  mutable std::mutex mutex_;
};

void apply_command(Emitter& emitter, Session& session, const Command& cmd) {
  if (cmd.name == "START") {
    if (cmd.fields.size() < 6) {
      emitter.Emit("COMMAND_RESULT", {"start", "0", "false", "missing-fields"});
      return;
    }
    CapturePlan plan;
    plan.sourceKind = resolve_source_kind(parse_source_kind(cmd.fields[0]));
    plan.sourceHandle = cmd.fields[1];
    plan.sourceWidth = std::atoi(cmd.fields[2].c_str());
    plan.sourceHeight = std::atoi(cmd.fields[3].c_str());
    plan.fps = std::atoi(cmd.fields[4].c_str());
    plan.bitrateKbps = std::atoi(cmd.fields[5].c_str());
    plan.audioSystem = cmd.fields.size() > 6 && cmd.fields[6] == "system";
    session.Start(plan);
  } else if (cmd.name == "STOP") {
    session.Stop("command");
  } else if (cmd.name == "PEER_ADD") {
    if (cmd.fields.empty()) return;
    session.AddPeer(cmd.fields[0]);
  } else if (cmd.name == "PEER_REMOVE") {
    if (cmd.fields.empty()) return;
    session.RemovePeer(cmd.fields[0]);
  } else if (cmd.name == "REMOTE_DESC") {
    if (cmd.fields.size() < 3) return;
    session.HandleRemoteDesc(cmd.fields[0], cmd.fields[1], cmd.fields[2]);
  } else if (cmd.name == "REMOTE_ICE") {
    if (cmd.fields.size() < 4) return;
    session.HandleRemoteIce(cmd.fields[0], cmd.fields[3],
        std::atoi(cmd.fields[2].c_str()), cmd.fields[1]);
  } else if (cmd.name == "SET_BITRATE") {
    if (cmd.fields.empty()) return;
    session.SetBitrate(std::atoi(cmd.fields[0].c_str()));
  } else if (cmd.name == "SET_FPS") {
    if (cmd.fields.empty()) return;
    session.SetFps(std::atoi(cmd.fields[0].c_str()));
  } else if (cmd.name == "SET_SCALE") {
    if (cmd.fields.empty()) return;
    session.SetScale(std::atoi(cmd.fields[0].c_str()));
  } else if (cmd.name == "SHUTDOWN") {
    session.Stop("shutdown");
    std::exit(0);
  } else {
    emitter.Emit("ERROR", {"unknown-command", "1", cmd.name});
  }
}

void stdin_loop(Emitter& emitter, Session& session, CommandQueue& queue) {
  std::string line;
  char buf[1024];
  while (std::cin.getline(buf, sizeof(buf))) {
    line.assign(buf);
    if (line.empty()) continue;
    if (line.size() > kMaxProtocolLineSize) {
      emitter.Emit("ERROR", {"protocol-line-too-long", "1", std::to_string(line.size())});
      continue;
    }
    Command cmd = parse_command(line);
    if (cmd.name.empty()) continue;
    queue.Push(std::move(cmd));
  }
  queue.Close();
}

void dispatch_loop(Emitter& emitter, Session& session, CommandQueue& queue) {
  while (true) {
    auto cmd = queue.PopFor(100);
    if (!cmd) {
      session.CheckDeadlines();
      continue;
    }
    apply_command(emitter, session, *cmd);
  }
}

}  // namespace haven

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  haven::Emitter emitter;
  if (argc == 2 && std::string(argv[1]) == "--probe") {
    return haven::run_probe(emitter);
  }
  haven::CommandQueue queue;
  haven::Session session(emitter, queue);
  std::thread reader(&haven::stdin_loop, std::ref(emitter), std::ref(session), std::ref(queue));
  haven::dispatch_loop(emitter, session, queue);
  reader.join();
  return 0;
}
