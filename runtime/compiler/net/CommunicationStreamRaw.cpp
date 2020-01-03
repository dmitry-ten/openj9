#include "control/CompilationRuntime.hpp"
#include "net/CommunicationStreamRaw.hpp"

namespace JITServer
{
uint32_t CommunicationStreamRaw::CONFIGURATION_FLAGS = 0;

void CommunicationStreamRaw::initVersion()
   {
   if (TR::Compiler->target.is64Bit() && TR::Options::useCompressedPointers())
      {
      CONFIGURATION_FLAGS |= JITServerCompressedRef;
      }
   CONFIGURATION_FLAGS |= JAVA_SPEC_VERSION & JITServerJavaVersionMask;
   }

}
