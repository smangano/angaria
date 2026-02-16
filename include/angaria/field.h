#pragma once
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
      MaybeHasFldType(FieldType ft) :
        fieldType(ft)
      {
      }

      MaybeHasFldType(const MaybeHasFldType& other) = default ;
      MaybeHasFldType& operator=(const MaybeHasFldType& other) = default ;
      void writeType(std::ostream & os) const {os.write(static_cast<const char*>(&fieldType), sizeof(fieldType); }
      void readType(std::istream & is) {is.read(static_cast<char*>(&fieldType), sizeof(fieldType); }
      FieldType fieldType {};
    } ;

    template <typename FLD>
    struct MaybeHasFldType<false, FLD>
    {
      MaybeHasFldType(FieldType ft) 
      {
      }

      MaybeHasFldType(const MaybeHasFldType& other) = default ;
      MaybeHasFldType& operator=(const MaybeHasFldType& other) = default ;
      void writeType(std::ostream & os) const {}
      void readType(std::istream & is) {}
    } ;

    constexpr bool INCLUDE_FLD = true ;

    template <FieldType FT, typename FLD>
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
          writeType(os) ;
          static_cast<const FLD*>(this)->writeImpl(os) ;
        }
         
        void read(std::istream & is) 
        {
          readType(os) ;
          static_cast<const FLD*>(this)->readImpl(os) ;
        }

        void registerField(Registry& registry) const
        {
            static_cast<const FLD*>(this)->registerFieldImpl(registry) ;
        } 
    } ;
  
   
    template <FieldType FT, typename T>  
    class IntegerField :  public Field<FT, IntegerField<T>> 
    {
    public:
       
        using Self = IntegerField<FT, T> ;
        using Type = T ;
        using Base = Field<FT, IntegerField<T>> ; 

        IntegerField(Registry& registry, EMsgType msgType, std::string name, std::string doc) : 
          Field(registry, msgType)
        {
          EFieldBase base = type_to_base<Type>.value ;
          registry.registerField(FT, std::move(name), base, std::move(doc)) ;
        }

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

        Self& operator +=(T v) { _value += v; return *this }
        Self& operator -=(T v) { _value -= v; return *this }
        Self& operator *=(T v) { _value *= v; return *this }
        Self& operator /=(T v) { _value /= v; return *this }

        T operator +(Self f, T v) { return f.get() + v ; }
        T operator -(Self f, T v) { return f.get() - v ; }
        T operator *(Self f, T) { return f.get() * v ; } 
        T operator /(Self f, T) { return f.get()  / v ; }

        auto operator<=>(const Self&) const = default;

        static inline bool operator==(Self f, T v) { return f.get() == v ; }
        static inline bool operator!=(Self f, T v) { return f.get() != v ; }
        static inline bool operator<=(Self f, T v) { return f.get() <= v ; }
        static inline bool operator>=(Self f, T v) { return f.get() >= v ; }
        static inline bool operator<(Self f, T v) { return f.get() < v ; }
        static inline bool operator>(Self f, T v) { return f.get() > v ;  

        void writeImpl(std::ostream & os) const
        {
          os.write(static_cast<const char*>(&_value), sizeof(_value)) ;
        }

        void readImpl(std::istream & is) const
        {
          is.read(static_cast<char*>(&_value), sizeof(_value)) ;
        }
        
        void registerFieldImpl(Registry& registry) const
        {
          registry.registerField(FLT, 
        }
    private:

      T _value ;               
    };

}
