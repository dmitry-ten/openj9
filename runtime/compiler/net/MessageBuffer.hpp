#ifndef MESSAGE_BUFFER_H
#define MESSAGE_BUFFER_H

#include <stdlib.h>
#include "net/Message.hpp"

namespace JITServer
{
class MessageBuffer
   {
public:
   MessageBuffer() :
      _capacity(10000),
      {
      _storage = static_cast<char *>(malloc(_capacity));
      _curPtr = _storage;
      }

   ~MessageBuffer()
      {
      free(_storage);
      }


   uint32_t size();
   char *getBufferStart() { return _storage; }

   template <typename T>
   void writeValue(const T &val)
      {
      writeData(&val, sizeof(T));
      }

   void writeData(void *dataStart, uint32_t dataSize);

   template <typename T>
   T *readValue()
      {
      if (_curPtr + sizeof(T) >= size())
         {
         TR_ASSERT(0, "Reading beyond message boundary");
         }
      T *valuePtr = reinterpret_cast<T *>(_curPtr);
      _curPtr += sizeof(T);
      }

private:
   void expandIfNeeded(uint32_t requiredSize);
   uint32_t _capacity;
   char *_storage;
   char *_curPtr;
   };
};
#endif
