#include "angaria/field.h"
#include <gtest/gtest.h>
namespace anagaria
{


  enum class EFieldType : uint16_t
  {
    TestField1,
    TestField2,
    TestField3
  } ;
}

TEST(Field, Int32)
{
  using Fld1 = Int32Filed<TestField1> ;

  Fld1 fld1(100) ;
}
