#pragma once

#include "angaria/types.h"
#include <compare>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <type_traits>

namespace angaria
{
  template <bool Enable, typename FLD>
  struct MaybeHasFldType;

  template <typename FLD>
  struct MaybeHasFldType<true, FLD>
  {
    MaybeHasFldType(EFieldType ft) : _fieldType(ft)
    {
    }

    MaybeHasFldType(const MaybeHasFldType& other) = default;
    MaybeHasFldType& operator=(const MaybeHasFldType& other) = default;
    auto operator<=>(const MaybeHasFldType&) const = default;

    void write_type(std::ostream& stream) const
    {
      const auto type = static_cast<std::underlying_type_t<EFieldType>>(_fieldType);
      stream.write(reinterpret_cast<const char*>(&type), sizeof(type));
    }

    void read_type(std::istream& stream)
    {
      std::underlying_type_t<EFieldType> type{};
      if (stream.read(reinterpret_cast<char*>(&type), sizeof(type)))
      {
        _fieldType = static_cast<EFieldType>(type);
      }
    }

    EFieldType _fieldType{};
  };

  template <typename FLD>
  struct MaybeHasFldType<false, FLD>
  {
    MaybeHasFldType(EFieldType)
    {
    }

    MaybeHasFldType(const MaybeHasFldType& other) = default;
    MaybeHasFldType& operator=(const MaybeHasFldType& other) = default;
    auto operator<=>(const MaybeHasFldType&) const = default;
    void write_type(std::ostream&) const
    {
    }
    void read_type(std::istream&)
    {
    }
  };

  constexpr bool INCLUDE_FLD = true;

  template <EFieldType FT, typename FLD>
  class Field : public MaybeHasFldType<INCLUDE_FLD, FLD>
  {
  public:
    using Base = MaybeHasFldType<INCLUDE_FLD, FLD>;

    Field() : Base(FT)
    {
    }

    auto operator<=>(const Field&) const = default;

    std::size_t size() const
    {
      return sizeof(*this);
    }
    std::size_t data_size() const
    {
      return sizeof(FLD);
    }
    std::string to_string() const
    {
      return static_cast<const FLD*>(this)->to_string_impl();
    }
    void write(std::ostream& os) const
    {
      Base::write_type(os);
      static_cast<const FLD*>(this)->write_impl(os);
    }

    void read(std::istream& is)
    {
      Base header = *this;
      header.read_type(is);
      if (!is)
      {
        return;
      }

      static_cast<FLD*>(this)->read_impl(is);
      if (is)
      {
        static_cast<Base&>(*this) = header;
      }
    }

  };

  template <EFieldType FT, typename T>
  class IntegerField : public Field<FT, IntegerField<FT, T>>
  {
  public:
    using Self = IntegerField<FT, T>;
    using Type = T;
    using Base = Field<FT, IntegerField<FT, T>>;

    IntegerField(T value) : Base(), _value(value)
    {
    }

    IntegerField(const IntegerField<FT, T>& other) = default;
    IntegerField<FT, T>& operator=(const IntegerField<FT, T>& other) = default;

    T get() const
    {
      return _value;
    }
    void set(T v)
    {
      _value = v;
    }

    Self& operator++()
    {
      ++_value;
      return *this;
    }
    Self& operator--()
    {
      --_value;
      return *this;
    }

    Self& operator+=(T v)
    {
      _value += v;
      return *this;
    }
    Self& operator-=(T v)
    {
      _value -= v;
      return *this;
    }
    Self& operator*=(T v)
    {
      _value *= v;
      return *this;
    }
    Self& operator/=(T v)
    {
      _value /= v;
      return *this;
    }

    friend T operator+(Self f, T v)
    {
      return f.get() + v;
    }
    friend T operator-(Self f, T v)
    {
      return f.get() - v;
    }
    friend T operator*(Self f, T v)
    {
      return f.get() * v;
    }
    friend T operator/(Self f, T v)
    {
      return f.get() / v;
    }

    auto operator<=>(const Self&) const = default;

    friend inline bool operator==(Self f, T v)
    {
      return f.get() == v;
    }
    friend inline bool operator!=(Self f, T v)
    {
      return f.get() != v;
    }
    friend inline bool operator<=(Self f, T v)
    {
      return f.get() <= v;
    }
    friend inline bool operator>=(Self f, T v)
    {
      return f.get() >= v;
    }
    friend inline bool operator<(Self f, T v)
    {
      return f.get() < v;
    }
    friend inline bool operator>(Self f, T v)
    {
      return f.get() > v;
    }

    std::string to_string_impl() const
    {
      return std::to_string(_value);
    }

    void write_impl(std::ostream& os) const
    {
      os.write(reinterpret_cast<const char*>(&_value), sizeof(_value));
    }

    void read_impl(std::istream& stream)
    {
      T value{};
      if (stream.read(reinterpret_cast<char*>(&value), sizeof(value)))
      {
        _value = value;
      }
    }

  private:
    T _value;
  };

  template <EFieldType FT>
  class Int32Field : public IntegerField<FT, int32_t>
  {
  public:
    using Base = IntegerField<FT, int32_t>;
    Int32Field(int32_t value) : Base(value)
    {
    }
  };
} // namespace angaria
