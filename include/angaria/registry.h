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
    LCTOFF_t  createTime ;          //The time the task registered with angoria as an offset to the LCT
    LCTOFF_t  lastCreateTime ;      //The time the task was last registered. Different from createTime due to restarts.
    PID_t     lastPID;              //The process id or thread id of the currently active task. 
    bool      active ;              //True if the Task identified by lastPID is actively running and registered with angoria
    uint16_t  restartCount ;        //The number of restarts for the task. Zero on initial task registration 
    VARCHAR_t doc ;                 //A doc string describing the task. Empty if not supplied by the task or configuration 
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
    uint64_t maxMessageCount ;
    VARCHAR_t doc ;
  } ;

  /**
  * Registry meta data for a Queue Set - a collection of queues that a Task can use to 
  * implement prioritized reads or multi task publishing.
  */
  struct RegistryQueueSet
  {
    QSID_t queueSetID ;
    char queueSetName[NAMELEN] ;
    EQSetTpe queueSetType;
    QID_t membership[MAX_QUEUES_PER_SET];
    uint8_t priority[MAX_QUEUES_PER_SET] ;
    LCTOFF_t createTime;
    TID_t createdByTask;
    TID_t ownedByTask; 
    VARCHAR_t doc;
  } ;

  /** 
  * Registry meta data for a Message - the basic unit of data that is transmitted between tasks 
  * via queues.
  */

  struct RegistryMessage
  {
    EMsgType msgType ;
    char msgName[NAMELEN] ;
    LCTOFF_t createTime ;
    uint32_t msgVersion; 
    TID_t createTaskID; 
    VARCHAR_t doc ;
  } ;

  /** 
  * Registry meta data desribing the fields that make up a message. 
  */
  struct RegistryMessageDef
  {
    EMsgType msgType ;
    uint16_t fieldCount ;
    EFieldType filelds[];
  } ;

  /**
  * Registry meta data describing a specific field in a message.
  */
  struct RegistryField
  {
    EFieldType fieldType ;
    char fieldName [NAMELEN];
    std::size_t fldSize ; 
    EFieldBase fldBaseType ;
    VARCHAR_t doc ;
  } ;

  class Registry 
  {
  public:
    using NamedMutex = boost::interprocess::named_mutex ;
    using NamedMutexPtr = std::unique_ptr<NamedMutex> ;

    Registry() {}

    inline
    static Registry& instance()
    {
      static Registry& theRegistry ;
      return theRegistry ;
    }
  
  
    TID_t registerTask(const std::string& name, PID_t pid, const std::string& doc = "") ;     

    QID_t registerQueue(const std::string& name, TID_t owner, const std::string& doc = "") ;     
    QID_t registerBoundedQueue(const std::string& name, TID_t owner, uint64_t maxMsgs, const std::string& doc = "") ;     

    /**
    * Register a Queue Set 
    * name - the unique name of the queue set.
    * type - either a EQSetTpe::SOURCE or a EQSetTpe::PUBLISHER
    * creator - the task ID of the task creating the queue set. Also becomes the owner until another task adopts the queue set.
    * doc - An optional description of how this queue set is used in the application. 
    */
    QSID_t registerQueueSet(const std::string& name, EQSetTpe type, TID_t creator, const std::string& doc = "") ;

    /**
    * After a queue `set is created but before it is used the queue set can be adopted by another task.
    * This can only happen once during the lifetime of the application
    * 
    * throws NoSuchQueueSetError if qsetID is not known
    * throws NoSuchTaskError of ownwerTask is not known
    * throws NotAllowed  if another task has already taken ownership (other than the creator)
    */
    void adoptQueueSet(QSID_t qsetID, TID_t ownwerTask) ;

    /**
     * Add a queue to a source queueset with specified priority.
     *
     * qsetID - the queue set to modify
     * qid - the queue to add 
     * priority - the priority of the queue within the queue set. Larger means more priority.
     *
     * throws NoSuchQueueSetError if qsetID is not known
     * throws NoSuchQueueError of qid does not exist
     * throws NotAllowed if qid is already a member of another queue set
     * throws NotAllowed if queue set is a publisher
     */
    void addQueueAsSource(QSID_t qsetID, QID_t qid, uint8_t priority) ; 

    /**
    * Add a queue to a publisher queue set 
    *
    * qsetID - the queue set to modify
    * qid - the queue to add
    * 
    * throws NoSuchQueueSetError if qsetID is not known
    * throws NoSuchQueueError of qid does not exist
    * throws NotAllowed if qid is already a member of another queue set
    * throws NotAllowed if queue set is a source
    */ 
    void addQueueAsDest(QSID_t qsetID, QID_t qid) ; 


  private:
    
  
  };
}   
