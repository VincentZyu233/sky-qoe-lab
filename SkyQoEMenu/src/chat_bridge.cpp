#include "chat_bridge.h"

#include "game_state.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skyqoe {
namespace {

constexpr std::uint32_t kAddChatMessageRva = 0x013F88C0;
constexpr std::uint32_t kOnKeyboardCompleteRva = 0x010E1670;
constexpr std::uint32_t kGameRootVtableRva = 0x01E1F070;
constexpr std::size_t kMessageCapacity = 256;
constexpr std::size_t kCaptureTextBytes = 512;
constexpr std::size_t kSendTextBytes = 256;
constexpr std::size_t kQueueCapacity = 8;
constexpr std::size_t kTaskCapacity = 128;
constexpr std::uint64_t kDispatchIntervalMs = 1200;
constexpr std::uint64_t kRateWindowMs = 10000;
constexpr std::size_t kRateWindowMaximum = 5;

constexpr std::array<std::uint8_t, 16> kAddChatMessagePrefix = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
    0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0x68,
};
constexpr std::array<std::uint8_t, 16> kOnKeyboardCompletePrefix = {
    0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81,
    0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0xAC,
};
constexpr std::array<const char*, 26> kChannelNames = {
    "public", "private", "local", "bench1", "bench2", "bench3", "bench4",
    "bench5", "bench6", "bench7", "bench8", "bench9", "bench10", "bench11",
    "bench12", "table", "chat1", "chat2", "chat3", "chat4", "chat5", "chat6",
    "chat7", "chat8", "audiencebroadcast", "loopback",
};

struct SourceUuid {
  std::array<std::uint8_t, 16> bytes{};
  std::uint32_t suffix = 0;
};

struct InternalChatMessage {
  std::uint64_t sequence = 0;
  std::uint64_t received_at_ms = 0;
  std::array<char, kCaptureTextBytes + 1> text{};
  std::uint16_t text_length = 0;
  SourceUuid source;
  std::uint32_t channel = 0;
  std::uint64_t sender_avatar = 0;
  std::int32_t sender_avatar_index = -1;
  bool local = false;
  bool sender_known = false;
};

struct PendingChatTask {
  std::uint64_t id = 0;
  std::string message;
};

using AddChatMessageFn = void (*)(void*, const char*, const SourceUuid*, std::uint32_t,
                                  const void*);
using OnKeyboardCompleteFn = void (*)(void*, char*, bool, bool);

std::uint8_t* g_module_base = nullptr;
void* g_add_chat_target = nullptr;
AddChatMessageFn g_original_add_chat = nullptr;
OnKeyboardCompleteFn g_on_keyboard_complete = nullptr;
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_game_thread_ready{false};
std::atomic<std::uint32_t> g_hook_inflight{0};
std::atomic<bool> g_capture_supported{false};
std::atomic<bool> g_capture_hook_installed{false};
std::atomic<bool> g_send_supported{false};

std::mutex g_message_mutex;
std::array<InternalChatMessage, kMessageCapacity> g_messages;
std::size_t g_message_write = 0;
std::size_t g_message_count = 0;
std::uint64_t g_next_message_sequence = 1;
std::uint64_t g_captured = 0;
std::uint64_t g_capture_dropped = 0;

std::mutex g_task_mutex;
std::deque<PendingChatTask> g_pending;
std::unordered_map<std::uint64_t, ChatTaskSnapshot> g_tasks;
std::deque<std::uint64_t> g_task_order;
std::deque<std::uint64_t> g_rate_accept_ticks;
std::uint64_t g_next_task_id = 1;
std::uint64_t g_next_dispatch_tick = 0;
std::uint64_t g_submitted = 0;
std::uint64_t g_failed = 0;
std::string g_status = "not initialized";

std::uint64_t UnixMilliseconds() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

bool ReadCurrent(const void* address, void* output, std::size_t size) {
  if (!address || !output || size == 0) {
    return false;
  }
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), address, output, size, &read) != FALSE &&
         read == size;
}

template <typename T>
bool ReadCurrent(const void* address, T& output) {
  return ReadCurrent(address, &output, sizeof(output));
}

bool IsContinuation(std::uint8_t value) {
  return (value & 0xC0U) == 0x80U;
}

bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
      codepoint = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t part = 1; part < length; ++part) {
      const std::uint8_t value = static_cast<std::uint8_t>(text[index + part]);
      if (!IsContinuation(value)) {
        return false;
      }
      codepoint = (codepoint << 6U) | (value & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
        codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

std::string SanitizeUtf8(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(text[index]);
    std::size_t length = first <= 0x7FU ? 1 : first <= 0xDFU ? 2 : first <= 0xEFU ? 3 : 4;
    const std::size_t remaining = text.size() - index;
    const std::size_t candidate = std::min(length, remaining);
    if (IsValidUtf8(text.substr(index, candidate))) {
      output.append(text.substr(index, candidate));
      index += candidate;
    } else {
      output += "\xEF\xBF\xBD";
      ++index;
    }
  }
  return output;
}

bool CopyCString(const char* source, std::array<char, kCaptureTextBytes + 1>& output,
                 std::size_t& length) {
  if (!source) {
    return false;
  }
  for (length = 0; length < kCaptureTextBytes; ++length) {
    char value = 0;
    if (!ReadCurrent(source + length, value)) {
      return false;
    }
    output[length] = value;
    if (value == '\0') {
      return true;
    }
  }
  output[kCaptureTextBytes] = '\0';
  return true;
}

std::string UuidHex(const SourceUuid& source) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(49);
  for (std::size_t index = 0; index < source.bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output.push_back('-');
    }
    output.push_back(kHex[source.bytes[index] >> 4U]);
    output.push_back(kHex[source.bytes[index] & 0x0FU]);
  }
  output.push_back(':');
  for (int shift = 28; shift >= 0; shift -= 4) {
    output.push_back(kHex[(source.suffix >> shift) & 0x0FU]);
  }
  return output;
}

const char* ChannelName(std::uint32_t channel) {
  return channel < kChannelNames.size() ? kChannelNames[channel] : "unknown";
}

void CaptureChatMessage(const char* text, const SourceUuid* source, std::uint32_t channel) {
  std::array<char, kCaptureTextBytes + 1> raw{};
  std::size_t raw_length = 0;
  SourceUuid source_copy;
  if (!CopyCString(text, raw, raw_length) || !source || !ReadCurrent(source, source_copy)) {
    std::scoped_lock lock(g_message_mutex);
    ++g_capture_dropped;
    return;
  }
  const std::string clean = SanitizeUtf8(std::string_view(raw.data(), raw_length));
  const GameSnapshot snapshot = GetGameState().Snapshot();

  InternalChatMessage message;
  message.received_at_ms = UnixMilliseconds();
  message.text_length = static_cast<std::uint16_t>(
      std::min<std::size_t>(clean.size(), message.text.size() - 1));
  std::memcpy(message.text.data(), clean.data(), message.text_length);
  message.text[message.text_length] = '\0';
  message.source = source_copy;
  message.channel = channel;
  for (const auto& player : snapshot.world.room_players) {
    if (!player.uuid_valid || player.uuid != source_copy.bytes) {
      continue;
    }
    message.sender_known = true;
    message.sender_avatar = player.avatar;
    message.sender_avatar_index = static_cast<std::int32_t>(player.index);
    message.local = player.local;
    break;
  }

  std::scoped_lock lock(g_message_mutex);
  message.sequence = g_next_message_sequence++;
  g_messages[g_message_write] = message;
  g_message_write = (g_message_write + 1) % g_messages.size();
  g_message_count = std::min(g_message_count + 1, g_messages.size());
  ++g_captured;
}

struct HookScope {
  HookScope() { g_hook_inflight.fetch_add(1, std::memory_order_acq_rel); }
  ~HookScope() { g_hook_inflight.fetch_sub(1, std::memory_order_acq_rel); }
};

void HookedAddChatMessage(void* game, const char* text, const SourceUuid* source,
                          std::uint32_t channel, const void* context) {
  HookScope scope;
  if (!g_shutting_down.load(std::memory_order_acquire)) {
    CaptureChatMessage(text, source, channel);
  }
  if (g_original_add_chat) {
    g_original_add_chat(game, text, source, channel, context);
  }
}

void SetTaskStateLocked(std::uint64_t id, const char* state, std::string detail) {
  const auto found = g_tasks.find(id);
  if (found == g_tasks.end()) {
    return;
  }
  found->second.state = state;
  found->second.detail = std::move(detail);
  found->second.updated_at_ms = UnixMilliseconds();
}

void TrimTasksLocked() {
  while (g_task_order.size() > kTaskCapacity) {
    const std::uint64_t id = g_task_order.front();
    const auto found = g_tasks.find(id);
    if (found != g_tasks.end() &&
        (found->second.state == "queued" || found->second.state == "running")) {
      break;
    }
    g_task_order.pop_front();
    g_tasks.erase(id);
  }
}

}  // namespace

bool ValidateChatMessage(std::string_view message, std::string& error_code,
                         std::string& error) {
  if (message.empty()) {
    error_code = "empty_message";
    error = "message must not be empty";
    return false;
  }
  if (message.size() > kSendTextBytes) {
    error_code = "message_too_long";
    error = "message exceeds the 256-byte UTF-8 limit";
    return false;
  }
  if (!IsValidUtf8(message)) {
    error_code = "invalid_utf8";
    error = "message must be valid UTF-8";
    return false;
  }
  for (const unsigned char value : message) {
    if (value == 0 || value == '\r' || value == '\n' || (value < 0x20U && value != '\t')) {
      error_code = "invalid_message_character";
      error = "message contains a prohibited control character";
      return false;
    }
  }
  error_code.clear();
  error.clear();
  return true;
}

bool InitializeChatBridge() {
  g_shutting_down.store(false, std::memory_order_release);
  g_game_thread_ready.store(false, std::memory_order_release);
  g_module_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  const BuildInfo build = GetGameState().Snapshot().build;
  if (!g_module_base || !build.supported) {
    std::scoped_lock lock(g_task_mutex);
    g_status = "unsupported Sky build; chat bridge disabled";
    return false;
  }

  std::array<std::uint8_t, kOnKeyboardCompletePrefix.size()> send_prefix{};
  g_on_keyboard_complete = reinterpret_cast<OnKeyboardCompleteFn>(
      g_module_base + kOnKeyboardCompleteRva);
  g_send_supported.store(
      ReadCurrent(reinterpret_cast<const void*>(g_on_keyboard_complete), send_prefix) &&
          send_prefix == kOnKeyboardCompletePrefix,
      std::memory_order_release);

  g_add_chat_target = g_module_base + kAddChatMessageRva;
  std::array<std::uint8_t, kAddChatMessagePrefix.size()> capture_prefix{};
  g_capture_supported.store(ReadCurrent(g_add_chat_target, capture_prefix) &&
                                capture_prefix == kAddChatMessagePrefix,
                            std::memory_order_release);
  if (g_capture_supported.load(std::memory_order_acquire)) {
    const MH_STATUS created = MH_CreateHook(g_add_chat_target,
                                            reinterpret_cast<void*>(&HookedAddChatMessage),
                                            reinterpret_cast<void**>(&g_original_add_chat));
    if (created == MH_OK && MH_EnableHook(g_add_chat_target) == MH_OK) {
      g_capture_hook_installed.store(true, std::memory_order_release);
    } else if (created == MH_OK) {
      MH_RemoveHook(g_add_chat_target);
      g_original_add_chat = nullptr;
    }
  }

  std::scoped_lock lock(g_task_mutex);
  if (g_send_supported.load(std::memory_order_acquire) &&
      g_capture_hook_installed.load(std::memory_order_acquire)) {
    g_status = "ready; native chat capture and queued send are available";
  } else if (g_send_supported.load(std::memory_order_acquire)) {
    g_status = "send ready; chat capture hook is unavailable";
  } else {
    g_status = "native chat signatures did not match";
  }
  return g_send_supported.load(std::memory_order_acquire) &&
         g_capture_hook_installed.load(std::memory_order_acquire);
}

void SetChatGameThreadReady(bool ready) {
  g_game_thread_ready.store(ready && g_send_supported.load(std::memory_order_acquire) &&
                                !g_shutting_down.load(std::memory_order_acquire),
                            std::memory_order_release);
}

void ShutdownChatBridge() {
  g_shutting_down.store(true, std::memory_order_release);
  g_game_thread_ready.store(false, std::memory_order_release);
  {
    std::scoped_lock lock(g_task_mutex);
    while (!g_pending.empty()) {
      SetTaskStateLocked(g_pending.front().id, "failed", "menu is shutting down");
      g_pending.pop_front();
      ++g_failed;
    }
    g_status = "shutting down";
  }
  if (g_capture_hook_installed.load(std::memory_order_acquire) && g_add_chat_target) {
    MH_DisableHook(g_add_chat_target);
  }
  while (g_hook_inflight.load(std::memory_order_acquire) != 0) {
    Sleep(1);
  }
  if (g_capture_hook_installed.load(std::memory_order_acquire) && g_add_chat_target) {
    MH_RemoveHook(g_add_chat_target);
  }
  std::scoped_lock lock(g_task_mutex);
  g_capture_hook_installed.store(false, std::memory_order_release);
  g_original_add_chat = nullptr;
  g_on_keyboard_complete = nullptr;
  g_add_chat_target = nullptr;
  g_status = "shut down";
}

ChatEnqueueResult QueueChatMessage(std::string message) {
  ChatEnqueueResult result;
  if (!ValidateChatMessage(message, result.error_code, result.error)) {
    return result;
  }
  std::scoped_lock lock(g_task_mutex);
  if (g_shutting_down.load(std::memory_order_acquire) ||
      !g_send_supported.load(std::memory_order_acquire) ||
      !g_game_thread_ready.load(std::memory_order_acquire)) {
    result.error_code = "chat_not_ready";
    result.error = "native chat send is not ready on the game thread";
    return result;
  }
  if (g_pending.size() >= kQueueCapacity) {
    result.error_code = "queue_full";
    result.error = "chat send queue is full";
    return result;
  }

  const std::uint64_t now_tick = GetTickCount64();
  while (!g_rate_accept_ticks.empty() &&
         now_tick - g_rate_accept_ticks.front() >= kRateWindowMs) {
    g_rate_accept_ticks.pop_front();
  }
  if (!g_rate_accept_ticks.empty() && now_tick - g_rate_accept_ticks.back() < kDispatchIntervalMs) {
    result.retry_after_ms = static_cast<std::uint32_t>(
        kDispatchIntervalMs - (now_tick - g_rate_accept_ticks.back()));
    result.error_code = "rate_limited";
    result.error = "chat send requests must be at least 1200 ms apart";
    return result;
  }
  if (g_rate_accept_ticks.size() >= kRateWindowMaximum) {
    result.retry_after_ms = static_cast<std::uint32_t>(
        kRateWindowMs - (now_tick - g_rate_accept_ticks.front()));
    result.error_code = "rate_limited";
    result.error = "at most five chat send requests are accepted per 10 seconds";
    return result;
  }

  const std::uint64_t task_id = g_next_task_id++;
  const std::uint64_t now_ms = UnixMilliseconds();
  ChatTaskSnapshot task;
  task.id = task_id;
  task.state = "queued";
  task.detail = "waiting for the verified game-thread hook";
  task.created_at_ms = now_ms;
  task.updated_at_ms = now_ms;
  g_tasks.emplace(task_id, task);
  g_task_order.push_back(task_id);
  g_pending.push_back({task_id, std::move(message)});
  g_rate_accept_ticks.push_back(now_tick);
  TrimTasksLocked();
  g_status = "chat send queued";

  result.accepted = true;
  result.task_id = task_id;
  result.queue_depth = static_cast<std::uint32_t>(g_pending.size());
  return result;
}

void ProcessPendingChatSend() {
  if (g_shutting_down.load(std::memory_order_acquire) ||
      !g_game_thread_ready.load(std::memory_order_acquire) || !g_on_keyboard_complete) {
    return;
  }
  const std::uint64_t now_tick = GetTickCount64();
  PendingChatTask request;
  {
    std::scoped_lock lock(g_task_mutex);
    if (g_pending.empty() || now_tick < g_next_dispatch_tick) {
      return;
    }
    request = std::move(g_pending.front());
    g_pending.pop_front();
    g_next_dispatch_tick = now_tick + kDispatchIntervalMs;
    SetTaskStateLocked(request.id, "running", "calling the native keyboard completion path");
  }

  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::uint64_t vtable = 0;
  std::array<std::uint8_t, kOnKeyboardCompletePrefix.size()> prefix{};
  const bool valid = snapshot.build.supported && snapshot.world.root >= 0x10000 &&
                     ReadCurrent(reinterpret_cast<const void*>(snapshot.world.root), vtable) &&
                     vtable == snapshot.build.module_base + kGameRootVtableRva &&
                     ReadCurrent(reinterpret_cast<const void*>(g_on_keyboard_complete), prefix) &&
                     prefix == kOnKeyboardCompletePrefix;
  if (!valid) {
    std::scoped_lock lock(g_task_mutex);
    ++g_failed;
    SetTaskStateLocked(request.id, "failed", "GameRoot or native chat signature validation failed");
    g_status = "chat send failed validation";
    return;
  }

  g_on_keyboard_complete(reinterpret_cast<void*>(snapshot.world.root), request.message.data(),
                         false, true);
  std::scoped_lock lock(g_task_mutex);
  ++g_submitted;
  SetTaskStateLocked(request.id, "succeeded", "submitted through Game::OnKeyboardComplete");
  g_status = "chat message submitted on the game thread";
}

ChatStatusSnapshot GetChatStatusSnapshot() {
  ChatStatusSnapshot snapshot;
  snapshot.capture_supported = g_capture_supported.load(std::memory_order_acquire);
  snapshot.capture_hook_installed =
      g_capture_hook_installed.load(std::memory_order_acquire);
  snapshot.send_supported = g_send_supported.load(std::memory_order_acquire);
  snapshot.game_thread_ready = g_game_thread_ready.load(std::memory_order_acquire);
  snapshot.shutting_down = g_shutting_down.load(std::memory_order_acquire);
  {
    std::scoped_lock lock(g_message_mutex);
    snapshot.messages_stored = static_cast<std::uint32_t>(g_message_count);
    snapshot.captured = g_captured;
    snapshot.capture_dropped = g_capture_dropped;
    if (g_message_count != 0) {
      const std::size_t oldest =
          (g_message_write + g_messages.size() - g_message_count) % g_messages.size();
      const std::size_t newest =
          (g_message_write + g_messages.size() - 1) % g_messages.size();
      snapshot.oldest_sequence = g_messages[oldest].sequence;
      snapshot.newest_sequence = g_messages[newest].sequence;
    }
  }
  {
    std::scoped_lock lock(g_task_mutex);
    snapshot.queue_depth = static_cast<std::uint32_t>(g_pending.size());
    snapshot.submitted = g_submitted;
    snapshot.failed = g_failed;
    snapshot.status = g_status;
  }
  return snapshot;
}

std::vector<ChatMessageSnapshot> GetChatMessages(std::uint64_t after, std::size_t limit) {
  limit = std::clamp<std::size_t>(limit, 1, 100);
  std::vector<ChatMessageSnapshot> result;
  std::scoped_lock lock(g_message_mutex);
  const std::size_t oldest =
      (g_message_write + g_messages.size() - g_message_count) % g_messages.size();
  for (std::size_t index = 0; index < g_message_count; ++index) {
    const InternalChatMessage& item = g_messages[(oldest + index) % g_messages.size()];
    if (item.sequence <= after) {
      continue;
    }
    ChatMessageSnapshot next;
    next.sequence = item.sequence;
    next.received_at_ms = item.received_at_ms;
    next.text.assign(item.text.data(), item.text_length);
    next.source_uuid = UuidHex(item.source);
    next.channel = item.channel;
    next.channel_name = ChannelName(item.channel);
    next.direction = item.sender_known ? (item.local ? "outgoing" : "incoming") : "unknown";
    next.sender_avatar = item.sender_avatar;
    next.sender_avatar_index = item.sender_avatar_index;
    result.push_back(std::move(next));
  }
  if (result.size() > limit) {
    result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
  }
  return result;
}

std::optional<ChatTaskSnapshot> GetChatTask(std::uint64_t id) {
  std::scoped_lock lock(g_task_mutex);
  const auto found = g_tasks.find(id);
  return found == g_tasks.end() ? std::nullopt
                                : std::optional<ChatTaskSnapshot>(found->second);
}

}  // namespace skyqoe
