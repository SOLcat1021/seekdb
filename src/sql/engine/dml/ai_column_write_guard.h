#pragma once
#include <atomic>
#include "lib/time/ob_time_utility.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{

// AI results are maintained by the server, not by user DML.  The guard also
// prevents the maintenance UPDATE from enqueueing another AI column task.
extern thread_local bool ai_column_internal_write;
extern thread_local const char *ai_column_system_prompt;

inline uint64_t next_ai_column_request_id()
{
  static std::atomic<uint64_t> last_id(0);
  const uint64_t clock_id = static_cast<uint64_t>(common::ObTimeUtility::current_time()) * 256;
  uint64_t previous = last_id.load(std::memory_order_relaxed);
  uint64_t next = 0;
  do {
    next = clock_id > previous ? clock_id : previous + 1;
  } while (!last_id.compare_exchange_weak(previous, next,
            std::memory_order_relaxed, std::memory_order_relaxed));
  return next;
}

class AiColumnWriteGuard
{
public:
  explicit AiColumnWriteGuard(const char *system_prompt = nullptr,
                              ObSQLSessionInfo *session = nullptr)
      : previous_(ai_column_internal_write), previous_prompt_(ai_column_system_prompt),
        session_(session), previous_session_value_(nullptr == session ? false : session->is_ai_column_internal_write())
  {
    ai_column_internal_write = true;
    ai_column_system_prompt = system_prompt;
    if (nullptr != session_) {
      session_->set_ai_column_internal_write(true);
    }
  }
  ~AiColumnWriteGuard()
  {
    ai_column_internal_write = previous_;
    ai_column_system_prompt = previous_prompt_;
    if (nullptr != session_) {
      session_->set_ai_column_internal_write(previous_session_value_);
    }
  }

private:
  bool previous_;
  const char *previous_prompt_;
  ObSQLSessionInfo *session_;
  bool previous_session_value_;
  AiColumnWriteGuard(const AiColumnWriteGuard &) = delete;
  AiColumnWriteGuard &operator=(const AiColumnWriteGuard &) = delete;
};

} // namespace sql
} // namespace oceanbase
