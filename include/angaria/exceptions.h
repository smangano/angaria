#pragma once

#include "angaria/types.h"
#include <stdexcept>
#include <string>

namespace angaria
{

  class AngariaException : public std::runtime_error
  {
  public:
    AngariaException(std::string text) noexcept : std::runtime_error(std::move(text))
    {
    }

  } ;

  class NoSuchQueueSetError : public AngariaException 
  {
  public:
     
    NoSuchQueueSetError(QSID qsid) noexcept 
      : AngariaException("No such queue set"), qsid(qsid)
    {
    }

    QSID_t qsid ;
  } ;
     
  class NoSuchQueueError : public AngariaException
  {
  public:
      
    NoSuchQueueError(QID_t qid) noexcept 
      : AngariaException("No such queue"), qid(qid)
    {
    } 

    QID_t qid ;
  } ;

  class NotAllowed : public AngariaException
  {
  public:
    
    NotAllowed(std::string text) noexcept 
      : AngariaException(std::move(text)) 
    {
    }
  } ; 

}
