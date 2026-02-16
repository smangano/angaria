#include "angaria/field.h"
#include <gtest/gtest.h>
namespace angaria
{


  enum class EFieldType : uint16_t
  {
    TestField1,
    TestField2,
    TestField3
  } ;
}

using namespace angaria ;

TEST(Field, Int32)
{
  using Fld1 = Int32Field<EFieldType::TestField1> ;

  Fld1 fld1(100) ;
}
