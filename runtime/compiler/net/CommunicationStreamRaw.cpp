#include "control/CompilationRuntime.hpp"
#include "net/CommunicationStreamRaw.hpp"

namespace JITServer
{
uint32_t CommunicationStreamRaw::CONFIGURATION_FLAGS = 0;

void
CommunicationStreamRaw::initVersion()
   {
   if (TR::Compiler->target.is64Bit() && TR::Options::useCompressedPointers())
      {
      CONFIGURATION_FLAGS |= JITServerCompressedRef;
      }
   CONFIGURATION_FLAGS |= JAVA_SPEC_VERSION & JITServerJavaVersionMask;
   }

void
CommunicationStreamRaw::readMessage(Message &msg)
   {
   msg.clearForRead();
   // read message meta data,
   // which contains the message type
   // and the number of data points
   uint32_t serializedSize;
   readBlocking(serializedSize);
   uint32_t messageSize = serializedSize - sizeof(uint32_t);
   msg.getBuffer()->expandIfNeeded(serializedSize);
   readBlocking(msg.getBuffer()->getBufferStart() + sizeof(uint32_t), messageSize);

   msg.reconstruct();
   
   // fprintf(stderr, "readMessage numDataPoints=%d serializedSize=%ld\n", msg.getMetaData()->numDataPoints, serializedSize);
   }

void
CommunicationStreamRaw::writeMessage(Message &msg)
   {
   const char *serialMsg = msg.serialize();
   // fprintf(stderr, "writeMessage numDataPoints=%d serializedSize=%ld\n", msg.getMetaData()->numDataPoints, msg.serializedSize());
   writeBlocking(serialMsg, msg.serializedSize());
   msg.clearForWrite();
   }
}
