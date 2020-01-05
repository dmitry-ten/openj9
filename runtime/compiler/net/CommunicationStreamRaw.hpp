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
      _storage = static_cast<char *>(malloc(100000));
      _curPtr = _storage;
      }

   ~CommunicationStreamRaw()
      {
      // free(_storage);
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
      // if (_storage)
         // free(storage);
      msg.clear();
      // read message meta data,
      // which contains the message type
      // and the number of data points
      long serializedSize;
      readBlocking(serializedSize);
      // _storage = static_cast<char *>(malloc(serializedSize));
      ::read(getConnFD(), _storage, serializedSize);

      deserializeMessage(msg);
      
      // fprintf(stderr, "numDataPoints=%d serializedSize=%ld\n", msg.getMetaData().numDataPoints, serializedSize);
      }

   void writeMessage(Message &msg)
      {
      long serializedSize;
      const char *serialMsg = serializeMessage(msg, serializedSize);
      writeBlocking(serializedSize);
      ::write(getConnFD(), serialMsg, serializedSize);
      msg.clear();
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
   char *_storage;
   char *_curPtr;
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

   template <typename T>
   void deserializeValue(T &val)
      {
      memcpy(&val, _curPtr, sizeof(T));
      _curPtr += sizeof(T);
      }

   template <typename T>
   void serializeValue(const T &val)
      {
      memcpy(_curPtr, &val, sizeof(T));
      _curPtr += sizeof(T);
      }

   const char *serializeMessage(const Message &msg, long &serializedSize)
      {
      _curPtr = _storage;
      memcpy(_curPtr, &msg.getMetaData(), sizeof(Message::MessageMetaData));
      _curPtr += sizeof(Message::MessageMetaData);
      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         serializeDataPoint(msg.getDataPoint(i));
         }
      serializedSize = _curPtr - _storage;
      _curPtr = _storage;
      return _storage;
      }

   void deserializeMessage(Message &msg)
      {
      _curPtr = _storage;
      Message::MessageMetaData *metaData = reinterpret_cast<Message::MessageMetaData *>(_curPtr);
      msg.setMetaData(*metaData);
      _curPtr += sizeof(Message::MessageMetaData);

      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         msg.addDataPoint(deserializeDataPoint(), false);
         }
      _curPtr = _storage;
      }

   Message::DataPoint deserializeDataPoint()
      {
      Message::DataPoint::MetaData pMetaData;
      deserializeValue(pMetaData);
      // fprintf(stderr, "deserializeDataPoint type=%d size=%d\n", pMetaData.type, pMetaData.size);

      auto dPoint = Message::DataPoint(pMetaData);
      if (dPoint.isContiguous())
         {
         dPoint.data = _curPtr;
         _curPtr += pMetaData.size;
         }
      else
         {
         // data point is a series of nested data points, thus need to write
         // data of each data point individually, to avoid copying.
         // 1. The first thing in data is the number of inner data points
         uint32_t numInnerPoints;
         deserializeValue(numInnerPoints);
         *static_cast<uint32_t *>(dPoint.data) = numInnerPoints;
         Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dPoint.data) + 1);
         for (uint32_t i = 0; i < numInnerPoints; ++i)
            {
            dPoints[i] = deserializeDataPoint();
            }
         }
      return dPoint;
      }

   void serializeDataPoint(const Message::DataPoint &dPoint)
      {
      // fprintf(stderr, "serializedDataPoint type=%d size=%d\n", dPoint.metaData.type, dPoint.metaData.size);
      serializeValue(dPoint.metaData);
      // write the data described by the datapoint
      if (dPoint.isContiguous())
         {
         memcpy(_curPtr, dPoint.data, dPoint.metaData.size);
         _curPtr += dPoint.metaData.size;
         }
      else
         {
         // data point is a series of nested data points, thus need to write
         // data of each data point individually, to avoid copying.
         // 1. The first thing in data is the number of inner data points
         uint32_t numInnerPoints = *static_cast<uint32_t *>(dPoint.data);
         serializeValue(numInnerPoints);
         Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dPoint.data) + 1);
         for (uint32_t i = 0; i < numInnerPoints; ++i)
            {
            // 2. write each data point as usual
            serializeDataPoint(dPoints[i]);
            }
         }
      }
   };

};

#endif
