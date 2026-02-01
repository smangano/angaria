#include "angaria/types.h"
#include <cstring>
#include <string>

namespace angaria
{
  bool setVarChar(VARCHAR_t & dest, const std::string& src)
  {
    bool status = true ;
    size_t sz = src.size() ; 
    if (dest.len < sz)
    {
      sz = dest.len ;
      status = false ;
    } 
    memcpy(dest.text, src.c_str(), sz) ;
  }

}
