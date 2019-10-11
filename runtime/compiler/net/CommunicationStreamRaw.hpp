#ifndef COMMUNICATION_STREAM_RAW_H
#define COMMUNICATION_STREAM_RAW_H

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

   int getConnFD() { return _connfd; }

private:
   int _connfd;
   };
};

#endif
