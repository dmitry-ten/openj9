
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
   // Things required for converting data passed to type converter into
   // a format that we can simply write into the socket and vice versa.
   // 1. Receive tuple of data points in stream::write
   // 2. For every data point, convert it to a format acceptable by socket write
   //   - For primitive and trivially copyable data types no conversion needed
   //   - For strings, send the buffer containing chars
   //   - For vectors, send the buffer containing elements
   //   - For tuples, convert tuple to a vector and then send the vector
   //   This should cover all currently supported cases of data structures.
   // 3. Write message type to the socket
   // 4. Write the number of data points sent to the socket
   // 5. Before writing every data point, write metadata that should describe
   // the type of the data and its size.
   // 6. Write every serialized data point to the socket.
   //
   // On the receiving side:
   // 1. Read message type.
   // 2. Read number of data points
   // 3. For every data point, first read the metadata
   // 4. Read data point and convert it to its original data type.
   //   - for primitives and trivially copyable types we just cast to the type
   //   - for vectors, will need to allocate vector filled with received values
   //   - for tuples will need to create a tuple filled with received values
   // 5. Create a tuple of all deserialized data points and return it.
   //
   class JITServerMessage
      {
      enum DataType
         {
         INT32,
         INT64
         UINT32,
         UINT64,
         BOOL,
         STRING,
         OBJECT, // only trivially-copyable
         VECTOR,
         TUPLE
         };
private:
      uint16_t _messageType;
      uint32_t _messageSize;
      
      };

   template <typename... Args>
   std::tuple<Args...> getArgs(const JITServerMessage *message);
   template <typename... Args>
   void setArgs(JITServerMessage *message, Args... args);

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
