
#ifndef RAW_TYPE_CONVERT_H
#define RAW_TYPE_CONVERT_H

#include <cstdint>
#include <utility>
#include <type_traits>
#include "StreamExceptions.hpp"
#include "omrcomp.h"

class J9Class;
class TR_ResolvedJ9Method;

namespace JITServer
   {
   struct TempBuffer
      {
      J9Class *_declaringClass1;
      J9Class *_declaringClass2;
      UDATA _field1;
      UDATA _field2;
      };
   struct TempBuffer2
      {
      TR_ResolvedJ9Method *_resolvedMethod1;
      TR_ResolvedJ9Method *_resolvedMethod2;
      int32_t _cpIndex1;
      int32_t _cpIndex2;
      bool _isStatic;
      };
   };
#endif
