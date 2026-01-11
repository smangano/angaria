#pragma once
#include <cstdint>

namespace angaria 
{
    enum class FieldType : uint16_t;
    enum class MsgType : uint16_t ;
    class FieldInfo;
    class MsgInfo;
    class Registry;
    class Message;
    class MessageHdr;
    class Field;

    using TID_t = uint16_t;
    using BITMASK_t = uint32_t;
}
