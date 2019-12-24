#ifndef COMMUNICATION_STREAM_RAW_H
#define COMMUNICATION_STREAM_RAW_H

#include "net/Message.hpp"
#include <unistd.h>

namespace JITServer
{
class CommunicationStreamRaw
   {
public:
   CommunicationStreamRaw(int connfd)
      {
      _connfd = connfd;
      }

   ~CommunicationStreamRaw()
      {
      close(_connfd);
      }

   // readBlocking and writeBlocking are functions that directly read/write
   // passed object from/to the socket. For the object to be correctly written,
   // it needs to be contiguous.
   template <typename T>
   void readBlocking(T &val)
      {
      // fprintf(stderr, "readBlocking %d bytes\n", sizeof(T));
      int32_t bytesRead  = read(_connfd, &val, sizeof(T));
      if (bytesRead == -1)
         {
         fprintf(stderr, "readBlocking read -1 bytes\n");
         throw JITServer::StreamFailure("JITServer I/O error: read error");
         }
      }

   template <typename T>
   void writeBlocking(const T &val)
      {
      // fprintf(stderr, "writeBlocking %d bytes\n", sizeof(T));
      int bytesWritten = write(_connfd, &val, sizeof(T));
      if (bytesWritten == -1)
         {
         fprintf(stderr, "writeBlocking wrote -1 bytes\n");
         throw JITServer::StreamFailure("JITServer I/O error: write error");
         }
      }

   void readMessage(Message &msg)
      {
      // read message meta data,
      // which contains the message size
      // and the number of data points
      Message::MessageMetaData metaData;
      readBlocking(metaData);
      msg.setMetaData(metaData);

      for (int32_t i = 0; i < metaData.numDataPoints; ++i)
         {
         Message::DataPoint dPoint = readDataPoint();
         msg.addDataPoint(dPoint);
         }
      }

   void writeMessage(const Message &msg)
      {
      writeBlocking(msg.getMetaData());

      // write data points
      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         writeDataPoint(msg.getDataPoint(i));
         }
      }

   int getConnFD() { return _connfd; }

   
protected:
   int _connfd;
   ServerMessage _sMsg;
   ClientMessage _cMsg;

   Message::DataPoint readDataPoint()
      {
      // read data point meta data,
      // which contains the data point size
      // and the type of the data point
      Message::DataPoint::MetaData pMetaData;
      readBlocking(pMetaData);
      char *data = (char *) malloc(pMetaData.size);
      ::read(getConnFD(), (void *) data, pMetaData.size);
      return {pMetaData, (void *) data};
      }

   void writeDataPoint(const Message::DataPoint &dPoint)
      {
      writeBlocking(dPoint.metaData);
      // write the data described by the datapoint
      ::write(getConnFD(), dPoint.data, dPoint.metaData.size);
      }
   };
};

#endif
