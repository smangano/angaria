#pragma once

#include "angaria/constants.h"

#include <cstdint>
#include <ctime>

namespace angaria
{
  enum class EFieldType : uint16_t;
  enum class EMsgType : uint16_t;
  enum class EQSetType : uint16_t
  {
    SOURCE,
    PUBLISHER
  };
  // A field base type helps tools display message records.
  enum class EFieldBase : uint16_t
  {
    BASE_NONE,
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
    BASE_STRING,
    BASE_INT8 = 26
  };

  template <typename T>
  struct TypeToBase
  {
    static constexpr EFieldBase value = EFieldBase::BASE_NONE;
  };

  template <>
  struct TypeToBase<char>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_CHAR;
  };

  template <>
  struct TypeToBase<uint8_t>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_BYTE;
  };

  template <>
  struct TypeToBase<uint16_t>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_UINT16;
  };

  template <>
  struct TypeToBase<int32_t>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_INT32;
  };

  template <>
  struct TypeToBase<uint32_t>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_UINT32;
  };

  template <>
  struct TypeToBase<int64_t>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_INT64;
  };

  template <>
  struct TypeToBase<float>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_FLOAT32;
  };

  template <>
  struct TypeToBase<double>
  {
    static constexpr EFieldBase value = EFieldBase::BASE_FLOAT64;
  };

  struct RegistryTask;
  struct RegistryQueue;
  struct RegistryQueueSet;
  struct RegistryMessage;
  struct RegistryMessageDef;
  struct RegistryField;
  class Registry;
  struct Message;

  using TID_t = uint16_t;     // Task ID
  using QID_t = uint16_t;     // Queue ID
  using QSID_t = uint16_t;    // Queue Set ID
  using PID_t = uint32_t;     // Process ID
  using BITMASK_t = uint32_t; // Bit flags

  struct Timestamp
  {
    std::time_t _time;
    uint64_t _nanosecs;
  };
  using timestamp_t = Timestamp;
  using LCT_t = Timestamp;
  using LCTOFF_t = uint64_t;
  using NAME_t = char[NAMELEN];
  struct VarChar
  {
    uint16_t _length;
    uint8_t _text[];
  };
  using VARCHAR_t = VarChar;
} // namespace angaria
