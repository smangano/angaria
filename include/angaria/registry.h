#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <variant> 

namespace angaria 
{
    enum class FieldType : uint16_t ;
    enum class MsgType : uint16_t ;

    class FieldInfo 
    {
    public:
    
    using Ptr = std::shared_ptr<FieldInfo>;

        FieldInfo(FieldType fieldType)
        {}


    private:
        FieldType m_fieldType;
        std::string m_fieldName;    
        std::string m_fieldDescription;


    };

    class NumericFieldInfo : public FieldInfo 
    {

    public:
        NumericFieldInfo(FieldType fieldType,
                         std::variant<double, int64_t> minValue,
                         std::variant<double, int64_t> maxValue)
            : FieldInfo(fieldType),
              m_minValue(minValue),
              m_maxValue(maxValue)
        {}
    private:
        std::variant<double, int64_t> m_minValue;
        std::variant<double, int64_t> m_maxValue; 
    };

    
    class Registry {
    public:
    using MsgFieldMap = std::unordered_map<MsgType, std::vector<FieldType>>;
    using MsgMap

    void registerMsgType(MsgType msgType)
    {
        return *this;    
    }

    Registry& registerFieldType(MsgType msgType, FieldType fieldType)
    {
        registerMsgType(msgType);

        
    }

    std::unorde
    };
}   