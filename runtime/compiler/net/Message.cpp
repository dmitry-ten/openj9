#include "net/Message.hpp"

namespace JITServer
{
const char *
Message::serialize(uint32_t &serializedSize)
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
Message::deserialize()
   {
   _curPtr = _storage;
   Message::MessageMetaData *metaData = reinterpret_cast<Message::MessageMetaData *>(_curPtr);
   _msg->setMetaData(*metaData);
   _curPtr += sizeof(Message::MessageMetaData);

   for (int32_t i = 0; i < _msg->getMetaData().numDataPoints; ++i)
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
