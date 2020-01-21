#include "net/Message.hpp"

namespace JITServer
{
Message::Message()
   {
   // metadata is always in the beginning of the message
   _serializedSizeOffset = _buffer.reserveValue<uint32_t>();
   _metaDataOffset = _buffer.reserveValue<Message::MetaData>();
   }

Message::MetaData *
Message::getMetaData() const
   {
   return _buffer.getValueAtOffset<MetaData>(_metaDataOffset);
   }

void
Message::addData(const DataDescriptor &desc, const void *dataStart)
   {
   uint32_t descOffset = _buffer.writeValue(desc);
   _buffer.writeData(dataStart, desc.size);
   _descriptorOffsets.push_back(descOffset);
   }

uint32_t
Message::reserveDescriptor()
   {
   uint32_t descOffset = _buffer.reserveValue<DataDescriptor>();
   _descriptorOffsets.push_back(descOffset);
   return descOffset;
   }

Message::DataDescriptor *
Message::getDescriptor(size_t idx) const
   {
   uint32_t offset = _descriptorOffsets[idx];
   return _buffer.getValueAtOffset<DataDescriptor>(offset);
   }

Message::DataDescriptor *
Message::getLastDescriptor() const
   {
   return _buffer.getValueAtOffset<DataDescriptor>(_descriptorOffsets.back());
   }

void
Message::reconstruct()
   {
   // Assume that buffer is populated with data that defines a valid message
   // Reconstruct the message by setting correct meta data and pointers to descriptors
   _metaDataOffset = _buffer.readValue<MetaData>();
   for (uint16_t i = 0; i < getMetaData()->numDataPoints; ++i)
      {
      uint32_t descOffset = _buffer.readValue<DataDescriptor>();
      _descriptorOffsets.push_back(descOffset);
      // skip the actual data
      _buffer.readData(getLastDescriptor()->size);
      }
   }

const char *
Message::serialize()
   {
   *_buffer.getValueAtOffset<uint32_t>(_serializedSizeOffset) = _buffer.size();
   return _buffer.getBufferStart();
   }

void
Message::clearForRead()
   {
   _descriptorOffsets.clear();
   _buffer.clear();
   _metaDataOffset = 0;
   _serializedSizeOffset = _buffer.reserveValue<uint32_t>();
   }

void
Message::clearForWrite()
   {
   _descriptorOffsets.clear();
   _buffer.clear();
   _serializedSizeOffset = _buffer.reserveValue<uint32_t>();
   _metaDataOffset = _buffer.reserveValue<MetaData>();
   }
};
