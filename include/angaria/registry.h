#pragma once
#include "angaria/bip.h"
#include "angaria/types.h"
#include <memory>

/**
* The Registry stores meta data in shared memory that is accessible and writeable by any Task that registers as an angoria 
* participant. Once a tasks updates the registry in any way the change is permanent for the lifetime of the session.
*/

namespace angaria 
{

  /**
  * Registry meta data for a Task - an execution unit using angaria to communicate.
  */
  struct RegistryTask
  {
    TID_t     taskID ;              //Every task is assigned a unique task id when it registers with angaria
    char      taskName[NAMELEN] ;   //A human readable name for the task
    LCTOFF_t    createTime ;          //The time the task registered with angoria as an offset to the LCT
    LCTOFF_t    lastCreateTime ;      //The time the task was last registered. Different from createTime due to restarts.
    PID_t     lastPID;              //The process id or thread id of the currently active task. 
    bool      active ;              //True if the Task identified by lastPID is actively running and registered with angoria
    uint16_t  restartCount ;        //The number of restarts for the task. Zero on initial task registration 
    VARCHAR_t   doc ;                 //A doc string describing the task. Empty if not supplied by the task or configuration 
  } ;

  /**
  * Registry meta data for a Queue - the basic communication facility between Tasks
  */
  struct RegistryQueue
  {
    QID_t queueID ;
    char taskName[NAMELEN] ; 
    TID_t createdByTask ;
    QSID_t ownedByQueueSet ;
    LCTOFF_t createTime ;
    uint64_t maxMessageSize ;
    VARCHAR_t doc ;
  }

  /**
  * Registry meta data for a Queue Set - a collection of queues that a Task can use to 
  * implement prioritized reads or multi task publishing.
  */
  struct RegistryQueueSet
  {
    QSID_t queueSetID ;
    char queueSetName[NAMELEN] ;
    QSET_TYPE queueSetType;
    QID_t membership[MAX_QUEUES_PER_SET];
    uint8_t priority[MAX_QUEUES_PER_SET] ;
    LCTOFF_t createTime;
    TID_t createdByTask;
    TID_t ownedByTask; 
    VARCHAR_t doc;
  }

  /** 
  * Registry meta data for a Message - the basic unit of data that is transmitted between tasks 
  * via queues.
  */

  struct RegistryMessage
  {
    MSGTYPE msgType ;
    char msgName[NAMELEN] ;
    LCTOFF_t createTime ;
    uint32_t msgVersion; 
    TID createTaskID; 
    VARCHAR_t doc ;
  } ;

  /** 
  * Registry meta data desribing the fields that make up a message. 
  */
  struct RegistryMessageDef
  {
    MSGTYPE msgType ;
    uint16_t fieldCount ;
    FLDTYPE filelds[]
  } ;

  /**
  * Regisistry meta data describing a specific field in a message.
  */
  struct RegistryField
  {
    FLDTYPE flytype ;
    char fieldName [NAMELEN]
    size_t fldSize ;
    FLDBASE fldBaseType ;
    VARCHAR_t doc ;
  } ;

  class Registry 
  {
  public:
    using NamedMutex = boost::interprocess::named_mutex ;
    using NamedMutexPtr = std::unique_ptr<NamedMutex> ;

         


  private:
    
    
  };
}   
