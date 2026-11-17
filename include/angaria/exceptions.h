#pragma once

#include "angaria/types.h"
#include <stdexcept>
#include <string>
#include <utility>

namespace angaria
{

  class AngariaException : public std::runtime_error
  {
  public:
    AngariaException(std::string text) : std::runtime_error(std::move(text))
    {
    }
  };

  class NoSuchQueueSetError : public AngariaException
  {
  public:
    NoSuchQueueSetError(QSID_t qsid) : AngariaException("No such queue set"), _qsid(qsid)
    {
    }

    QSID_t _qsid;
  };

  class NoSuchQueueError : public AngariaException
  {
  public:
    NoSuchQueueError(QID_t qid) : AngariaException("No such queue"), _qid(qid)
    {
    }

    QID_t _qid;
  };

  class NotAllowed : public AngariaException
  {
  public:
    NotAllowed(std::string text) : AngariaException(std::move(text))
    {
    }
  };

} // namespace angaria
