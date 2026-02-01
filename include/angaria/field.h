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

    template <typename FLD>
    class Field : public MaybeHasFldType<INCLUDE_FLD, FLD>
    {
    
    private:

        static void registerFieldTypes(MsgType msgType, FieldType fieldType)
        {
            std::call_once(s_field_registration_flag, []() {
                
                
            });
        }   

    public:

        using Base = MaybeHasFldType<INCLUDE_FLD, FLD> ;

        Field(MsgType msgType, FieldType fieldType) : Base(fieldType) 
        {
          registerFieldTypes(msgType, fieldType) ;
        }

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

        static std::once_flag s_field_registration_flag;
    } ;
  
   
    template <typename T>  
    class IntegerField :  public Field<IntegerField<T>> 
    {
    public:
        
        IntegerField(MsgType msgType, FieldType fieldType, T value)
        {

        }
               
    };

}
