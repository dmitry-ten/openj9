#include "net/Message.hpp"

namespace JITServer
{
Message::Message()
   {
   // metadata is always in the beginning of the message
   _metaData = _buffer.reserveValue<Message::MetaData>();
   }

void
Message::addData(const DataDescriptor &desc, void *dataStart);
   {
   DataDescriptor *descPtr = _buffer.writeValue(desc);
   _buffer.writeData(dataStart, desc.size);
   _dataPoints.push_back(descPtr);
   _metaData->numDataPoints++;
   }

DataDescriptor *
Message::reserveDescriptor()
   {
   return _buffer.reserveValue(DataDescriptor);
   }

void
Message::reconstruct()
   {
   // Assume that buffer is populated with data that defines a valid message
   // Reconstruct the message by setting correct meta data and pointers to descriptors
   _metaData = _buffer.readValue<MetaData>();
   for (uint16_t i = 0; i < _metaData->numDataPoints; ++i)
      {
      DataDescriptor *curDesc = _buffer.readValue<DataDescriptor>();
      _descriptors.push_back(curDesc);
      // skip the actual data
      _buffer.readData(curDesc->size);
      }
   }

void
Message::clear()
   {
   _metaData->numDataPoints = 0;
   _metaData->type = 0;
   _metaData->version = 0;
   _descriptors.clear();
   _buffer.clear();
   _metaData = _buffer.reserveValue<Message::MetaData>();
   }
};
