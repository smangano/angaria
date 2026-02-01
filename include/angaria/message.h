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

        EMsgType m_msgType;
        uint32_t m_msgSize;
        timestamp m_enqueueTime;
        timestamp m_dequeueTime;
        TID_t m_enqueTID;
        TID_t m_dequeTID;
        BITMASK_t m_flags;
    };

    class Message 
    {
    public:

    private:
        MessageHdr m_header;
    };
}
