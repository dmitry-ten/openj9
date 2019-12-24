
#ifndef MESSAGE_H
#define MESSAGE_H

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
      OBJECT, // only trivially-copyable
      VECTOR,
      TUPLE
      };

   // These structs are needed because they are contiguous
   // and can be sent/read directly from the socket. This
   // allows us to serialize the message without extra copying.
   struct MessageMetaData
      {
      uint16_t numDataPoints; // number of data points in a message
      MessageType type;
      };

   struct DataPoint
      {
      struct MetaData
         {
         DataType type;
         uint32_t size;
         };
      MetaData metaData;
      void *data;
      };

   void setMetaData(const MessageMetaData &metaData)
      {
      _metaData = metaData;
      }

   MessageMetaData getMetaData() const { return _metaData; }

   void addDataPoint(const DataPoint &dataPoint)
      {
      _dataPoints.push_back(dataPoint);
      }

   const DataPoint &getDataPoint(size_t idx) const { return _dataPoints[idx]; }

   void setType(MessageType type) { _metaData.type = type; }

   MessageType type() const { return _metaData.type; }

   void clear()
      {
      _metaData.numDataPoints = 0;
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
   uint64_t version() { return _version; }
   void setVersion(uint64_t version) { _version = version; }
   void clearVersion() { _version = 0; }

private:
   uint64_t _version;   
   };
};
#endif
