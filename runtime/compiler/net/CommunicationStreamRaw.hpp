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
      // what is to be done with the memory allocation?
      // Ideally, want to use per-compilation region memory,
      // maybe not here, but definitely in RawTypeConvert.
      // But on the server side, the first message is received
      // before the compilation object is created, so cannot do that
      //
      // Could just use persistent memory, but then would need to 
      // free it manually. In the communication stream that should be fine,
      // since we are normally keeping the stream for multiple compilations,
      // so recycling the same persistent memory space should be fine, even if
      // we end up consuming more memory than ideally possible, we would cut down
      // on the number of allocations. Although I doubt that would matter a lot,
      // since the compilations themselves do a ton of allocations.
      // what's important is cutting down on the number of copies and tons of small allocations
      //
      // Another solution could be to create a separate per-compilation region on the server,
      // look at CompilationThread.cpp:7564 for example on how to do that.
      //
      // In any case, I need to resolve all other issues first and test if this actually
      // consumes less CPU before tackling that problem
      _storageSize = 10000;
      _storage = static_cast<char *>(malloc(_storageSize));
      _curPtr = _storage;
      }

   ~CommunicationStreamRaw()
      {
      free(_storage);
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
      msg.clear();
      // read message meta data,
      // which contains the message type
      // and the number of data points
      uint32_t serializedSize;
      readBlocking(serializedSize);
      uint32_t messageSize = serializedSize - sizeof(uint32_t);
      expandStorageIfNeeded(messageSize);
      readBlocking(_storage, messageSize);

      deserializeMessage(msg);
      
      // fprintf(stderr, "readMessage numDataPoints=%d serializedSize=%ld\n", msg.getMetaData().numDataPoints, serializedSize);
      }

   void writeMessage(Message &msg)
      {
      uint32_t serializedSize = 0;
      const char *serialMsg = serializeMessage(msg, serializedSize);
      // fprintf(stderr, "writeMessage numDataPoints=%d serializedSize=%ld\n", msg.getMetaData().numDataPoints, serializedSize);
      writeBlocking(serialMsg, serializedSize);
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
   uint32_t _storageSize;
   char *_curPtr;
   // readBlocking and writeBlocking are functions that directly read/write
   // passed object from/to the socket. For the object to be correctly written,
   // it needs to be contiguous.
   template <typename T>
   void readBlocking(T &val)
      {
      readBlocking(&val, sizeof(T));
      }

   template <typename T>
   void readBlocking(T *ptr, size_t size)
      {
      int32_t bytesRead = read(_connfd, ptr, size);
      if (bytesRead == -1)
         {
         throw JITServer::StreamFailure("JITServer I/O error: read error");
         }
      }

   template <typename T>
   void writeBlocking(const T &val)
      {
      writeBlocking(&val, sizeof(T));
      }

   template <typename T>
   void writeBlocking(T *ptr, size_t size)
      {
      int32_t bytesWritten = write(_connfd, ptr, size);
      if (bytesWritten == -1)
         {
         throw JITServer::StreamFailure("JITServer I/O error: read error");
         }
      }

   void expandStorageIfNeeded(uint32_t requiredSize)
      {
      if (requiredSize > _storageSize)
         {
         // deallocate current storage and reallocate it to fit the message
         free(_storage);
         _storageSize = requiredSize * 2;
         _storage = static_cast<char *>(malloc(_storageSize));
         fprintf(stderr, "expanded storage to %u\n", _storageSize);
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

   const char *serializeMessage(const Message &msg, uint32_t &serializedSize)
      {
      // need to be able to tell the serialized size before serializing,
      // otherwise cannot guarantee no memory overflow
      serializedSize = sizeof(uint32_t) + sizeof(Message::MessageMetaData);
      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         serializedSize += msg.getDataPoint(i).serializedSize();
         }
      expandStorageIfNeeded(serializedSize);

      // write total serialized size at the beginning of storage
      memcpy(_storage, &serializedSize, sizeof(uint32_t));
      _curPtr = _storage + sizeof(uint32_t);
      memcpy(_curPtr, &msg.getMetaData(), sizeof(Message::MessageMetaData));
      _curPtr += sizeof(Message::MessageMetaData);
      for (int32_t i = 0; i < msg.getMetaData().numDataPoints; ++i)
         {
         serializeDataPoint(msg.getDataPoint(i));
         }
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
      dPoint.data = _curPtr;
      if (dPoint.isContiguous())
         {
         _curPtr += pMetaData.size;
         }
      else
         {
         // data point is a series of nested data points, thus need to write
         // data of each data point individually, to avoid copying.
         // 1. The first thing in data is the number of inner data points
         uint32_t numInnerPoints = *static_cast<uint32_t *>(dPoint.data);
         _curPtr += sizeof(uint32_t);
         Message::DataPoint *dPoints = reinterpret_cast<Message::DataPoint *>(_curPtr);
         for (uint32_t i = 0; i < numInnerPoints; ++i)
            {
            Message::DataPoint dPoint = deserializeDataPoint();
            dPoints[i].data = dPoint.data;
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
