#pragma once
#include "angaria/types.h"
#include <ctime>

namespace angaria 
{
    struct timestamp
    {
        time_t seconds;
        uint64_t nanoseconds;
    };

    class MessageHdr 
    {
    public:
        MessageHdr(EMsgType msgType, uint32_t msgSize)
            : m_msgType(msgType), m_msgSize(msgSize), m_enqueueTime({0,0}), m_dequeueTime({0,0}),
              m_enqueTID(0), m_dequeTID(0), m_flags(0)      
        {}

        EMsgType getMsgType() const { return m_msgType; }
        uint32_t getMsgSize() const { return m_msgSize; }

    private:

        EMsgType _msgType;
        uint32_t _msgSize;
        timestamp _enqueueTime;
        timestamp _dequeueTime;
        TID_t _enqueTID;
        TID_t _dequeTID;
        BITMASK_t _flags;
    };

    class Message 
    {
    public:
      Message(MsgType msgType, uint32_t msgSize) :
      _header(msgType, msgSize) 
      {
      } 
    private:
        MessageHdr _header;
    };


}
