#pragma once

#include "angaria/types.h"
#include <ctime>

namespace angaria
{
  struct MessageTimestamp
  {
    std::time_t _seconds;
    uint64_t _nanoseconds;
  };

  class MessageHdr
  {
  public:
    MessageHdr(EMsgType msgType, uint32_t msgSize)
        : _msgType(msgType), _msgSize(msgSize)
    {
    }

    EMsgType get_msg_type() const
    {
      return _msgType;
    }
    uint32_t get_msg_size() const
    {
      return _msgSize;
    }

  private:
    EMsgType _msgType;
    uint32_t _msgSize;
    [[maybe_unused]] MessageTimestamp _enqueueTime{};
    [[maybe_unused]] MessageTimestamp _dequeueTime{};
    [[maybe_unused]] TID_t _enqueueTid{};
    [[maybe_unused]] TID_t _dequeueTid{};
    [[maybe_unused]] BITMASK_t _flags{};
  };

  class Message
  {
  public:
    Message(EMsgType msgType, uint32_t msgSize) : _header(msgType, msgSize)
    {
    }

  private:
    MessageHdr _header;
  };

} // namespace angaria
