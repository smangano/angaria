#pragma once
#include "angaria/registry.h"
#include "angaria/types.h"
#include <cstdint> 
#include <mutex>
#include <iostream>

namespace angaria 
{
 
    template <bool Enable, typename FLD>
    struct  MaybeHasFldType ;

    template <typename FLD>
    struct MaybeHasFldType<true, FLD>
    {
      MaybeHasFldType(EFieldType ft) :
        fieldType(ft)
      {
      }

      MaybeHasFldType(const MaybeHasFldType& other) = default ;
      MaybeHasFldType& operator=(const MaybeHasFldType& other) = default ;
      void writeType(std::ostream & os) const {auto tmp = std::underlying_type(fieldType); os.write(static_cast<const char*>(&tmp), sizeof(tmp)); }
      void readType(std::istream & is) {int16_t tmp; is.read(reinterpret_cast<char*>(&tmp), sizeof(tmp)); fieldType = static_cast<EFieldType>(tmp);}
      EFieldType fieldType {};
    } ;

    template <typename FLD>
    struct MaybeHasFldType<false, FLD>
    {
      MaybeHasFldType(EFieldType ft) 
      {
      }

      MaybeHasFldType(const MaybeHasFldType& other) = default ;
      MaybeHasFldType& operator=(const MaybeHasFldType& other) = default ;
      void writeType(std::ostream & os) const {}
      void readType(std::istream & is) {}
    } ;

    constexpr bool INCLUDE_FLD = true ;

    template <EFieldType FT, typename FLD>
    class Field : public MaybeHasFldType<INCLUDE_FLD, FLD>
    {
    public:

        using Base = MaybeHasFldType<INCLUDE_FLD, FLD> ;

        Field()  : Base(FT) {}


        size_t size() const {return sizeof(*this) ; }
        size_t dataSize() const {return sizeof(FLD) ; }
        std::string toString() const { return static_cast<const FLD*>(this)->toStringImpl() ; } 
        void write(std::ostream & os) const
        {
          Base::writeType(os) ;
          static_cast<const FLD*>(this)->writeImpl(os) ;
        }
         
        void read(std::istream & is) 
        {
          Base::readType(is) ;
          static_cast<const FLD*>(this)->readImpl(is) ;
        }

        void registerField(Registry& registry) const
        {
            static_cast<const FLD*>(this)->registerFieldImpl(registry) ;
        } 
    } ;
  
   
    template <EFieldType FT, typename T>  
    class IntegerField :  public Field<FT, IntegerField<FT,T>>
    {
    public:
       
        using Self = IntegerField<FT, T> ;
        using Type = T ;
        using Base = Field<FT, IntegerField<FT,T>> ;

        IntegerField(T value) :
          Base(), _value(value) 
        {
        }

        IntegerField(const IntegerField<FT,T>& other) = default ;
        IntegerField<FT,T>& operator=(const IntegerField<FT,T>& other) = default ;

        T get() const { return _value ;}
        void set(T v) const {_value  = v ;}

        Self& operator++() { ++_value; return *this;}
        Self& operator--() { --_value; return *this;}

        Self& operator +=(T v) { _value += v; return *this; }
        Self& operator -=(T v) { _value -= v; return *this; }
        Self& operator *=(T v) { _value *= v; return *this; }
        Self& operator /=(T v) { _value /= v; return *this; }

        friend T operator +(Self f, T v) { return f.get() + v ; }
        friend T operator -(Self f, T v) { return f.get() - v ; }
        friend T operator *(Self f, T) { return f.get() * v ; } 
        friend T operator /(Self f, T) { return f.get()  / v ; }

        auto operator<=>(const Self&) const = default;

        friend inline bool operator==(Self f, T v) { return f.get() == v ; }
        friend inline bool operator!=(Self f, T v) { return f.get() != v ; }
        friend inline bool operator<=(Self f, T v) { return f.get() <= v ; }
        friend inline bool operator>=(Self f, T v) { return f.get() >= v ; }
        friend inline bool operator<(Self f, T v) { return f.get() < v ; }
        friend inline bool operator>(Self f, T v) { return f.get() > v ;  }

        void writeImpl(std::ostream & os) const
        {
          os.write(static_cast<const char*>(&_value), sizeof(_value)) ;
        }

        void readImpl(std::istream & is) const
        {
          is.read(static_cast<char*>(&_value), sizeof(_value)) ;
        }
        
        void registerFieldImpl(Registry& registry, std::string name, std::string doc = "") const
        {
          registry.registerField(FT, std::move(name), type_to_base<T>().value, std::move(doc)) ;
        }

    private:

      T _value ;               
    };

    template <EFieldType FT>
    class Int32Field : public IntegerField<FT, int32_t>
    {
    public:

      using Base = IntegerField<FT, int32_t> ;
      Int32Field(int32_t value) : Base(value) {}
      
    } ;
}
