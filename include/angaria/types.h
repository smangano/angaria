#pragma once
#include "angaria/constants.h"
#include <cstdint>
#include <ctime>

namespace angaria 
{
  enum class EFieldType : uint16_t;
  enum class EMsgType : uint16_t ;
  enum class EQSetTpe : uint16_t { SOURCE, PUBLISHER } ;
  //User can optionally specify a field is a model of a specific type
  //This assits tools which display log messages
  enum class EFieldBase : uint16_t 
  { 
    BASE_NONE, //deafult if no base specified 
    BASE_CHAR, 
    BASE_BYTE, 
    BASE_INT16, 
    BASE_UINT16, 
    BASE_INT32, 
    BASE_UINT32, 
    BASE_INT64, 
    BASE_UINT64,  
    BASE_FLOAT32, 
    BASE_FLOAT64, 
    BASE_DATE, 
    BASE_TIMESTAMP,  
    BASE_YEARS, 
    BASE_MONTHS, 
    BASE_DAYS, 
    BASE_HOURS, 
    BASE_MINS, 
    BASE_SECS, 
    BASE_MILLISECS, 
    BASE_MICROSECS, 
    BASE_NANOSECS, 
    BASE_CHARS, 
    BASE_BYTES, 
    BASE_SYMBOL, 
    BASE_STRING 
    } ;

  template <typename T>
  struct type_to_base {
    static constexpr EFieldBase value = EFieldBase::BASE_NONE;
  };

  template <>
  struct type_to_base<char> {
    static constexpr EFieldBase value = EFieldBase::BASE_CHAR;
  };

  template <>
  struct type_to_base<uint8_t> {
    static constexpr EFieldBase value = EFieldBase::BASE_BYTE;
  };

  template <>
  struct type_to_base<uint16_t> {
    static constexpr EFieldBase value = EFieldBase::BASE_UINT16;
  };

  template <>
  struct type_to_base<int32_t> {
    static constexpr EFieldBase value = EFieldBase::BASE_INT32;
  };

  template <>
  struct type_to_base<uint32_t> {
    static constexpr EFieldBase value = EFieldBase::BASE_UINT32;
  };

  template <>
  struct type_to_base<int64_t> {
    static constexpr EFieldBase value = EFieldBase::BASE_INT64;
  };

  template <>
  struct type_to_base<float> {
    static constexpr EFieldBase value = EFieldBase::BASE_FLOAT32;
  };

  template <>
  struct type_to_base<double> {
    static constexpr EFieldBase value = EFieldBase::BASE_FLOAT64;
  };

  struct RegistryTask;
  struct RegistryQueue ;
  struct RegistryQueueSet ;
  struct RegistryMessage ;
  struct RegistryMessageDef ;
  struct RegistryField ;
  class Registry ;
  class Message ;
  class MessageHdr ;
  class Field ;

  using TID_t = uint16_t; 		    // Task ID
  using QID_t = uint16_t; 		    // Queue ID
  using QSID_t = uint16_t; 		    // Queue Set ID
  using PID_t = uint32_t ;        // Process ID
  using BITMASK_t = uint32_t;     // Bit flags 

  struct timestamp_t { std::time_t time; uint64_t nanosecs; };
  using LCT_t = timestamp_t ;   
  using LCTOFF_t = uint64_t ;   
  using NAME_t = char[NAMELEN] ; 
  struct VARCHAR_t { uint16_t len ; uint8_t text[] ; } ;
}
