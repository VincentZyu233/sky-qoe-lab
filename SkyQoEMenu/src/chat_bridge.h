#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skyqoe {

struct ChatMessageSnapshot {
  std::uint64_t sequence = 0;
  std::uint64_t received_at_ms = 0;
  std::string text;
  std::string source_uuid;
  std::uint32_t channel = 0;
  std::string channel_name;
  std::string direction;
  std::uint64_t sender_avatar = 0;
  std::int32_t sender_avatar_index = -1;
};

struct ChatTaskSnapshot {
  std::uint64_t id = 0;
  std::string state;
  std::string detail;
  std::uint64_t created_at_ms = 0;
  std::uint64_t updated_at_ms = 0;
};

struct ChatStatusSnapshot {
  bool capture_supported = false;
  bool capture_hook_installed = false;
  bool send_supported = false;
  bool game_thread_ready = false;
  bool shutting_down = false;
  std::uint32_t queue_depth = 0;
  std::uint32_t queue_capacity = 8;
  std::uint32_t messages_stored = 0;
  std::uint32_t message_capacity = 256;
  std::uint64_t oldest_sequence = 0;
  std::uint64_t newest_sequence = 0;
  std::uint64_t captured = 0;
  std::uint64_t capture_dropped = 0;
  std::uint64_t submitted = 0;
  std::uint64_t failed = 0;
  std::string status = "not initialized";
};

struct ChatEnqueueResult {
  bool accepted = false;
  std::uint64_t task_id = 0;
  std::uint32_t queue_depth = 0;
  std::uint32_t retry_after_ms = 0;
  std::string error_code;
  std::string error;
};

bool InitializeChatBridge();
void ShutdownChatBridge();
void SetChatGameThreadReady(bool ready);
void ProcessPendingChatSend();
ChatEnqueueResult QueueChatMessage(std::string message);
ChatStatusSnapshot GetChatStatusSnapshot();
std::vector<ChatMessageSnapshot> GetChatMessages(std::uint64_t after, std::size_t limit);
std::optional<ChatTaskSnapshot> GetChatTask(std::uint64_t id);
bool ValidateChatMessage(std::string_view message, std::string& error_code,
                         std::string& error);

}  // namespace skyqoe
