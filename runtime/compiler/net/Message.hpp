
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
   // i.e. number of data points and message type.
   // This is a struct instead of just being members of
   // Message class because we write/read metadata separately
   // from the message data, so having this struct is convenient.
   struct MessageMetaData
      {
      MessageMetaData() :
         numDataPoints(0),
         totalSize(0)
         {}

      uint16_t numDataPoints; // number of data points in a message
      long int totalSize; // total number of data bytes in the message
      MessageType type;
      };

   // Struct describing a single data point in a message.
   // Contains metadata describing data type and size
   // and a pointer to the beginning of the data.
   // data pointer 
   struct DataPoint
      {
      struct MetaData
         {
         DataType type;
         uint32_t size;
         };

      DataPoint(MetaData metaData, const void *data) :
         metaData(metaData),
         data((void *) data)
         {}

      DataPoint(DataType type, uint32_t size, const void *data) :
         data((void  *) data)
         {
         metaData.type = type;
         metaData.size = size;
         }

      DataPoint(MetaData metaData) :
         metaData(metaData),
         data(malloc(metaData.size))
         {}

      bool isContiguous() const { return metaData.type != VECTOR && metaData.type != TUPLE; }

      void freeStorage() { free(data); data = NULL; }

      MetaData metaData;
      void *data;
      };

   void setMetaData(MessageMetaData metaData)
      {
      _metaData = metaData;
      }

   const MessageMetaData &getMetaData() const { return _metaData; }

   void addDataPoint(const DataPoint &dataPoint, bool isWrite)
      {
      // this function needs to increment numDataPoints when called
      // for writing the message, but not when called for reading.
      //
      if (isWrite)
         {
         _metaData.numDataPoints++;

         }

      _dataPoints.push_back(dataPoint);
      }

   const DataPoint &getDataPoint(size_t idx) const { return _dataPoints[idx]; }

   void setType(MessageType type) { _metaData.type = type; }

   MessageType type() const { return _metaData.type; }

   void clear()
      {
      _metaData.numDataPoints = 0;
      _metaData.totalSize = 0;
      _dataPoints.clear();
      }


private:
   MessageMetaData _metaData;
   std::vector<DataPoint> _dataPoints;
   };


class ServerMessage : public Message
   {
   };

class ClientMessage : public Message
   {
public:
   uint64_t version() { return _version; }
   void setVersion(uint64_t version) { _version = version; }
   void clearVersion() { _version = 0; }

private:
   uint64_t _version;   
   };
};
#endif
