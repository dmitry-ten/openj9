#include "net/MessageBuffer.hpp"

namespace JITServer
{
uint32_t
MessageBuffer::size()
   {
   return _curPtr - _storage;
   }

void
MessageBuffer::expandIfNeeded(uint32_t requiredSize)
   {
   if (requiredSize > _capacity)
      {
      // deallocate current storage and reallocate it to fit the message,
      // copying the values
      _capacity = requiredSize * 2;
      char *newStorage = static_cast<char *>(malloc(_capacity));
      memcpy(newStorage, _storage, size());
      free(_storage);
      _storage = newStorage;
      fprintf(stderr, "\nExpanded message buffer to %u\n", _capacity);
      }
   }

void
MessageBuffer::writeData(void *dataStart, uint32_t dataSize)
   {
   // write size number of bytes starting from dataStart
   // into the buffer. Expand it if needed
   uint32_t requiredSize = size() + dataSize;
   expandIfNeeded(requiredSize);
   memcpy(_curPtr, dataStart, dataSize);
   _curPtr += dataSize;
   }

const char *
MessageBuffer::serializeMessage(uint32_t &serializedSize)
   {
   // need to be able to tell the serialized size before serializing,
   // otherwise cannot guarantee no memory overflow
   serializedSize = sizeof(uint32_t) + sizeof(Message::MessageMetaData);
   for (int32_t i = 0; i < _msg->getMetaData().numDataPoints; ++i)
      {
      serializedSize += _msg->getDataPoint(i).serializedSize();
      }
   expandIfNeeded(serializedSize);

   // write total serialized size at the beginning of storage
   _curPtr = _storage;
   serializeValue(serializedSize);
   serializeValue(_msg->getMetaData());
   for (int32_t i = 0; i < _msg->getMetaData().numDataPoints; ++i)
      {
      serializeDataPoint(_msg->getDataPoint(i));
      }
   _curPtr = _storage;
   return _storage;
   }

void
MessageBuffer::deserializeMessage()
   {
   _metaData = *_buffer.nextValue<Message::MessageMetaData>();

   for (int32_t i = 0; i < _metaData.numDataPoints; ++i)
      {
      _msg->addDataPoint(deserializeDataPoint(), false);
      }
   _curPtr = _storage;
   }

Message::DataPoint
MessageBuffer::deserializeDataPoint()
   {
   // data in _storage should already be in a format corresponding
   // to the datapoint, all we need to do is fix the data pointer
   // to point to the received data
   auto dPointPtr = reinterpret_cast<Message::DataPoint *>(_curPtr);
   _curPtr += sizeof(Message::DataPoint);
   // fix the data pointer
   void *dataStart = static_cast<void *>(_curPtr);
   dPointPtr->data = dataStart;
   // fprintf(stderr, "deserializeDataPoint type=%d size=%d data=%p\n", dPointPtr->type, dPointPtr->size, dPointPtr->data);
   if (dPointPtr->isContiguous())
      {
      // skip the data segment
      _curPtr += dPointPtr->size;
      }
   else
      {
      // data point is a series of nested data points
      // 1. find the number of nested data points
      uint32_t numInnerPoints = *reinterpret_cast<uint32_t *>(_curPtr);
      _curPtr += sizeof(uint32_t);
      for (uint32_t i = 0; i < numInnerPoints; ++i)
         {
         // 2. deserialize each nested data point
         // this will advance the _curPtr to the beginning of next nested point
         Message::DataPoint innerPoint = deserializeDataPoint();
         }
      }
   return *dPointPtr;
   }

void
MessageBuffer::serializeDataPoint(const Message::DataPoint &dPoint)
   {
   // fprintf(stderr, "serializedDataPoint type=%d size=%d\n", dPoint.type, dPoint.size);
   // serialize data point struct
   serializeValue(dPoint);
   if (dPoint.isContiguous())
      {
      // if contiguous, just copy value to directly after the metadata
      memcpy(_curPtr, dPoint.data, dPoint.size);
      _curPtr += dPoint.size;
      }
   else
      {
      // data point is a series of nested data points
      // 1. serialize the number of nested data points
      uint32_t numInnerPoints = *static_cast<uint32_t *>(dPoint.data);
      serializeValue(numInnerPoints);
      Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dPoint.data) + 1);
      for (uint32_t i = 0; i < numInnerPoints; ++i)
         {
         // 2. serialize each data point
         serializeDataPoint(dPoints[i]);
         }
      }
   }
};
