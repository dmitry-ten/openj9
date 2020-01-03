#ifndef COMMUNICATION_STREAM_RAW_H
#define COMMUNICATION_STREAM_RAW_H

#include "net/Message.hpp"
#include "env/TRMemory.hpp"
#include "env/CompilerEnv.hpp" // for TR::Compiler->target.is64Bit()
#include "control/Options.hpp" // TR::Options::useCompressedPointers()
#include <unistd.h>

namespace JITServer
{
class CommunicationStreamRaw
   {
protected:
   CommunicationStreamRaw()
      {
      }

   ~CommunicationStreamRaw()
      {
      close(_connfd);
      }

#if defined(JITSERVER_ENABLE_SSL)
   void initStream(int connfd, BIO *ssl)
#else
   void initStream(int connfd)
#endif
      {
      _connfd = connfd;
#if defined(JITSERVER_ENABLE_SSL)
      _ssl = ssl;
#endif
      }


   void readMessage(Message &msg)
      {
      msg.clear(true);
      // read message meta data,
      // which contains the message type
      // and the number of data points
      Message::MessageMetaData metaData;
      readBlocking(metaData);
      msg.setMetaData(metaData);

      // char *storage = static_cast<char *>(malloc(msg.getMetaData().totalSize));
      // ::read(getConnFD(), storage, msg.getMetaData().totalSize);
      
      // char *curPtr = storage;
      // read each data point into a Message object
      for (int32_t i = 0; i < metaData.numDataPoints; ++i)
         {
         Message::DataPoint dPoint = readDataPoint();
         // Message::DataPoint dPoint = *reinterpret_cast<Message::DataPoint *>(curPtr);
         // dPoint.data = curPtr + sizeof(Message::DataPoint);
         msg.addDataPoint(dPoint, false);
         // curPtr += sizeof(Message::DataPoint) + dPoint.metaData.size;
         }
      fprintf(stderr, "numDataPoints=%d totalSize=%d\n", msg.getMetaData().numDataPoints, msg.getMetaData().totalSize);
      }

   void writeMessage(Message &msg)
      {
      // write message metadata
      writeBlocking(msg.getMetaData());

      // write data points
      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         writeDataPoint(msg.getDataPoint(i));
         }
      msg.clear(false);
      }

   int getConnFD() { return _connfd; }

   static void initVersion();

   static uint64_t getJITServerVersion()
      {
      return ((((uint64_t)CONFIGURATION_FLAGS) << 32) | (MAJOR_NUMBER << 24) | (MINOR_NUMBER << 8));
      }
   
   int _connfd;
   ServerMessage _sMsg;
   ClientMessage _cMsg;

   static const uint8_t MAJOR_NUMBER = 0;
   static const uint16_t MINOR_NUMBER = 1;
   static const uint8_t PATCH_NUMBER = 0;
   static uint32_t CONFIGURATION_FLAGS;

private:
   // readBlocking and writeBlocking are functions that directly read/write
   // passed object from/to the socket. For the object to be correctly written,
   // it needs to be contiguous.
   template <typename T>
   void readBlocking(T &val)
      {
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
      int bytesWritten = write(_connfd, &val, sizeof(T));
      if (bytesWritten == -1)
         {
         fprintf(stderr, "writeBlocking wrote -1 bytes\n");
         throw JITServer::StreamFailure("JITServer I/O error: write error");
         }
      }

   Message::DataPoint readDataPoint()
      {
      // read the data point metadata,
      // which contains the data point size and type.
      Message::DataPoint::MetaData pMetaData;
      readBlocking(pMetaData);
      // fprintf(stderr, "readDataPoint type=%d size=%d\n", pMetaData.type, pMetaData.size);

      // read the data contained in the datapoint.
      // dynamically allocates storage for the received data
      auto dPoint = Message::DataPoint(pMetaData);
      if (dPoint.isContiguous())
         {
         ::read(getConnFD(), dPoint.data, pMetaData.size);
         }
      else
         {
         // data point is a series of nested data points, thus need to write
         // data of each data point individually, to avoid copying.
         // 1. The first thing in data is the number of inner data points
         uint32_t numInnerPoints;
         readBlocking(numInnerPoints);
         *static_cast<uint32_t *>(dPoint.data) = numInnerPoints;
         Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dPoint.data) + 1);
         for (uint32_t i = 0; i < numInnerPoints; ++i)
            {
            dPoints[i] = readDataPoint();
            }
         }
      return dPoint;
      }

   void writeDataPoint(const Message::DataPoint &dPoint)
      {
      // fprintf(stderr, "writeDataPoint type=%d size=%d\n", dPoint.metaData.type, dPoint.metaData.size);
      writeBlocking(dPoint.metaData);
      // write the data described by the datapoint
      if (dPoint.isContiguous())
         {
         ::write(getConnFD(), dPoint.data, dPoint.metaData.size);
         }
      else
         {
         // data point is a series of nested data points, thus need to write
         // data of each data point individually, to avoid copying.
         // 1. The first thing in data is the number of inner data points
         uint32_t numInnerPoints = *static_cast<uint32_t *>(dPoint.data);
         writeBlocking(numInnerPoints);
         Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dPoint.data) + 1);
         for (uint32_t i = 0; i < numInnerPoints; ++i)
            {
            // 2. write each data point as usual
            writeDataPoint(dPoints[i]);
            }
         }
      }
   };

};

#endif
