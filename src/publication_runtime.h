#pragma once

#include "runtime_state.h"

#include <span>

namespace angaria::detail
{
  // Gate-bound native operations. Public wrappers authenticate the native Task
  // owner before acquiring RuntimeAccess; these functions check the captured
  // claim again against recovered state. Operation failures throw RuntimeFailure
  // except output sizing, whose report preserves the required capacity.
  PublicationReport runtime_send(RuntimeAccess& access, const TaskClaim& producer,
                                    QueueId queue, PublicationId publication, MessageView message,
                                    std::span<DestinationOutcome> outcomes);
  PublicationReport runtime_publication_status(RuntimeAccess& access, const TaskClaim& producer,
                                                  PublicationId publication,
                                                  std::span<DestinationOutcome> outcomes);
} // namespace angaria::detail
