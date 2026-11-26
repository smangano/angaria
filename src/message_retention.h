#pragma once

#include "angaria/message.h"

#include <utility>

namespace angaria::detail
{
  // Current scalar/logical Fields are self-contained. Retain their resolved
  // codec independently of Session handles; later pool-backed Fields extend
  // this private context without making a Message own a delivery pin.
  class MessageRetention
  {
  public:
    MessageRetention(SessionId session, BinaryPayloadCodec codec)
        : _session(session), _codec(std::move(codec))
    {
    }

    SessionId _session;
    BinaryPayloadCodec _codec;
  };

  struct MessageAccess
  {
    // The native receiver supplies authenticated Session/schema context. This
    // private binding does not take ownership of the borrowed payload bytes.
    static Result<MessageView> bind(MessageHeader header, std::span<const std::byte> payload,
                                      SessionId session, BinaryPayloadCodec codec);
  };
} // namespace angaria::detail
