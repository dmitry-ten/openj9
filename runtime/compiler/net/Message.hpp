
#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdlib.h>

namespace JITServer
{

class Message
   {
public:
   enum DataType
      {
      INT32,
      INT64,
      UINT32,
      UINT64,
      BOOL,
      STRING,
      OBJECT, // only trivially-copyable,
      ENUM,
      VECTOR,
      TUPLE
      };

   // Struct containing message metadata,
   // i.e. number of data points, type and message type.
   struct MetaData
      {
      MetaData() :
         numDataPoints(0)
         {}

      uint16_t numDataPoints; // number of data points in a message
      MessageType type;
      uint64_t version;
      };

   // Struct describing a single data point in a message.
   struct DataDescriptor
      {
      DataType type;
      uint32_t size; // size of the data segment, which can include nested data

      DataDescriptor(DataType type, uint32_t size) :
         type(type),
         size(size)
         {}

      bool isContiguous() const { return type != VECTOR && type != TUPLE; }

      // const uint32_t serializedSize() const
         // {
         // if (isContiguous())
            // {
            // return sizeof(MetaData) + metaData.size;
            // }
         // else
            // {
            // uint32_t numInnerPoints = *static_cast<uint32_t *>(data);
            // Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(data) + 1);
            // uint32_t totalSize = sizeof(MetaData) + sizeof(uint32_t) + metaData.size;
            // for (uint32_t i = 0; i < numInnerPoints; ++i)
               // {
               // totalSize += dPoints[i].serializedSize();
               // }
            // return totalSize;
            // }
         // }
      };

   void setMetaData(const MetaData &metaData)
      {
      MetaData *metaDataPtr = _buffer.writeValue(metaData);
      _metaData = metaDataPtr;
      }

   MetaData *getMetaData() const { return _metaData; }

   void addData(const DataDescriptor &desc, void *dataStart)
      {
      DataDescriptor *descPtr = _buffer.writeValue(desc);
      _buffer.writeData(dataStart, desc.size);
      _dataPoints.push_back(descPtr);
      _metaData->numDataPoints++;
      }

   DataDescriptor *getDescriptor(size_t idx) { return _descriptors[idx]; }

   void reconstruct()
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

   void setType(MessageType type) { _metaData->type = type; }

   MessageType type() const { return _metaData->type; }

   MessageBuffer *getBuffer() { return &_buffer; }

   void clear()
      {
      _metaData.numDataPoints = 0;
      _descriptors.clear();
      _buffer.clear();
      }


protected:
   MetaData *_metaData;
   std::vector<DataDescriptor *> _descriptors;
   MessageBuffer _buffer;
   };


class ServerMessage : public Message
   {
   };

class ClientMessage : public Message
   {
public:
   uint64_t version() { return _metaData.version; }
   void setVersion(uint64_t version) { _metaData.version = version; }
   void clearVersion() { _metaData.version = 0; }
   };
};
#endif
