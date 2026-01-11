#pragma once
#include <cstdint> 
#include <mutex>

namespace angaria 
{
 
    enum class FieldType : uint16_t ;
    enum class MsgType : uint16_t ;
    class Field {
    public:

        static void registerFieldTypes(MsgType msgType, FieldType fieldType)
        {
            std::call_once(s_field_registration_flag, []() {
                
                
            });
        }   

        Field(MsgType msgType, FieldType fieldType)
        {}

        static std::once_flag s_field_registration_flag;
    }
  
    template <FieldType FIELDTYPE, typename T>
    class NumericField {
    public:
        
        NumericField(MsgType msgType, FieldType fieldType, T value)
        {

        }
               
    };

}