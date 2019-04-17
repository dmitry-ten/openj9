/*******************************************************************************
 * Copyright (c) 2018, 2019 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "rpc/J9Server.hpp"
#include "j9methodServer.hpp"
#include "control/CompilationRuntime.hpp"
#include "control/CompilationThread.hpp"
#include "control/MethodToBeCompiled.hpp"
#include "control/JITaaSCompilationThread.hpp"
#include "exceptions/DataCacheError.hpp"

ClientSessionData::ClassInfo &
getJ9ClassInfo(TR::CompilationInfoPerThread *threadCompInfo, J9Class *clazz)
   {
   // This function assumes that you are inside of _romMapMonitor
   // Do not use it otherwise
   auto &classMap = threadCompInfo->getClientData()->getROMClassMap();
   auto it = classMap.find(clazz);
   TR_ASSERT(it != classMap.end(), "ClassInfo is not in the class map");
   return it->second;
   }

static J9ROMMethod *
romMethodAtClassIndex(J9ROMClass *romClass, uint64_t methodIndex)
   {
   J9ROMMethod * romMethod = J9ROMCLASS_ROMMETHODS(romClass);
   for (size_t i = methodIndex; i; --i)
      romMethod = nextROMMethod(romMethod);
   return romMethod;
   }

TR_ResolvedJ9JITaaSServerMethod::TR_ResolvedJ9JITaaSServerMethod(TR_OpaqueMethodBlock * aMethod, TR_FrontEnd * fe, TR_Memory * trMemory, TR_ResolvedMethod * owningMethod, uint32_t vTableSlot)
   : TR_ResolvedJ9Method(fe, owningMethod),
   _fieldAttributesCache(decltype(_fieldAttributesCache)::allocator_type(trMemory->heapMemoryRegion())),
   _staticAttributesCache(decltype(_staticAttributesCache)::allocator_type(trMemory->heapMemoryRegion())),
   _resolvedMethodsCache(decltype(_resolvedMethodsCache)::allocator_type(trMemory->heapMemoryRegion()))
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
   TR::CompilationInfo *compInfo = TR::CompilationInfo::get(fej9->getJ9JITConfig());
   TR::CompilationInfoPerThread *threadCompInfo = compInfo->getCompInfoForThread(fej9->vmThread());
   _stream = threadCompInfo->getMethodBeingCompiled()->_stream;

   // Create client side mirror of this object to use for calls involving RAM data
   TR_ResolvedJ9Method* owningMethodMirror = owningMethod ? ((TR_ResolvedJ9JITaaSServerMethod*) owningMethod)->getRemoteMirror() : nullptr;

   // If in AOT mode, will actually create relocatable version of resolved method on the client
   _stream->write(JITaaS::J9ServerMessageType::mirrorResolvedJ9Method, aMethod, owningMethodMirror, vTableSlot, fej9->isAOT_DEPRECATED_DO_NOT_USE());
   auto recv = _stream->read<TR_ResolvedJ9JITaaSServerMethodInfo>();
   auto &methodInfo = std::get<0>(recv);

   unpackMethodInfo(aMethod, fe, trMemory, vTableSlot, threadCompInfo, methodInfo);
   }

TR_ResolvedJ9JITaaSServerMethod::TR_ResolvedJ9JITaaSServerMethod(TR_OpaqueMethodBlock * aMethod, TR_FrontEnd * fe, TR_Memory * trMemory, const TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo, TR_ResolvedMethod * owningMethod, uint32_t vTableSlot)
   : TR_ResolvedJ9Method(fe, owningMethod),
   _fieldAttributesCache(decltype(_fieldAttributesCache)::allocator_type(trMemory->heapMemoryRegion())),
   _staticAttributesCache(decltype(_staticAttributesCache)::allocator_type(trMemory->heapMemoryRegion())),
   _resolvedMethodsCache(decltype(_resolvedMethodsCache)::allocator_type(trMemory->heapMemoryRegion()))
   {
   // Mirror has already been created, its parameters are passed in methodInfo
   TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
   TR::CompilationInfo *compInfo = TR::CompilationInfo::get(fej9->getJ9JITConfig());
   TR::CompilationInfoPerThread *threadCompInfo = compInfo->getCompInfoForThread(fej9->vmThread());
   _stream = threadCompInfo->getMethodBeingCompiled()->_stream;
   unpackMethodInfo(aMethod, fe, trMemory, vTableSlot, threadCompInfo, methodInfo);
   }

J9ROMClass *
TR_ResolvedJ9JITaaSServerMethod::romClassPtr()
   {
   return _romClass;
   }

J9RAMConstantPoolItem *
TR_ResolvedJ9JITaaSServerMethod::literals()
   {
   return _literals;
   }

J9Class *
TR_ResolvedJ9JITaaSServerMethod::constantPoolHdr()
   {
   return _ramClass;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isJNINative()
   {
   return _isJNINative;
   }

TR_ResolvedJ9Method *
TR_ResolvedJ9JITaaSServerMethod::getRemoteMirror()
   {
   recreateRemoteMirrorIfNecessary();
   return _remoteMirror;
   }

void
TR_ResolvedJ9JITaaSServerMethod::setRecognizedMethodInfo(TR::RecognizedMethod rm)
   {
   TR_ResolvedJ9Method::setRecognizedMethodInfo(rm);
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_setRecognizedMethodInfo, getRemoteMirror(), rm);
   _stream->read<JITaaS::Void>();
   }

J9ClassLoader *
TR_ResolvedJ9JITaaSServerMethod::getClassLoader()
   {
   return _classLoader; // cached version
   }

bool
TR_ResolvedJ9JITaaSServerMethod::staticAttributes(TR::Compilation * comp, I_32 cpIndex, void * * address, TR::DataType * type, bool * volatileP, bool * isFinal, bool * isPrivate, bool isStore, bool * unresolvedInCP, bool needAOTValidation)
   {
   bool isStatic = true;
   TR_J9MethodFieldAttributes attributes;
   if(!getCachedFieldAttributes(cpIndex, attributes, isStatic))
      {
      // make a remote call, if attributes are not cached
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_staticAttributes, getRemoteMirror(), cpIndex, isStore, needAOTValidation, getResolvedMethodParams());
      auto recv = _stream->read<TR_J9MethodFieldAttributes, TR_ResolvedJ9Method *>();
      attributes = std::get<0>(recv);

      cacheFieldAttributes(cpIndex, attributes, isStatic);
      if (!_remoteMirror)
         _remoteMirror = std::get<1>(recv);
      }
   else
      {
      TR_ASSERT(validateMethodFieldAttributes(attributes, isStatic, cpIndex, isStore, needAOTValidation), "static attributes from client and cache do not match");
      }

   bool result;
   attributes.setMethodFieldAttributesResult(address, type, volatileP, isFinal, isPrivate, unresolvedInCP, &result);

   return result;
   }

TR_OpaqueClassBlock *
TR_ResolvedJ9JITaaSServerMethod::getClassFromConstantPool(TR::Compilation * comp, uint32_t cpIndex, bool returnClassForAOT)
   {
   if (cpIndex == -1)
      return nullptr;

   TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
      {
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor());
      auto &constantClassPoolCache = getJ9ClassInfo(compInfoPT, _ramClass)._constantClassPoolCache;
      if (!constantClassPoolCache)
         {
         // initialize cache, called once per ram class
         constantClassPoolCache = new (PERSISTENT_NEW) PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *>(PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *>::allocator_type(TR::Compiler->persistentAllocator()));
         }
      else
         {
         auto it = constantClassPoolCache->find(cpIndex);
         if (it != constantClassPoolCache->end())
            return it->second;
         }
      }
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getClassFromConstantPool, getRemoteMirror(), cpIndex, returnClassForAOT);
   TR_OpaqueClassBlock *resolvedClass = std::get<0>(_stream->read<TR_OpaqueClassBlock *>());
   if (resolvedClass)
      {
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor());
      auto constantClassPoolCache = getJ9ClassInfo(compInfoPT, _ramClass)._constantClassPoolCache;
      constantClassPoolCache->insert({cpIndex, resolvedClass});
      }
   return resolvedClass;
   }

TR_OpaqueClassBlock *
TR_ResolvedJ9JITaaSServerMethod::getDeclaringClassFromFieldOrStatic(TR::Compilation *comp, int32_t cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getDeclaringClassFromFieldOrStatic, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<TR_OpaqueClassBlock *>());
   }

TR_OpaqueClassBlock *
TR_ResolvedJ9JITaaSServerMethod::classOfStatic(I_32 cpIndex, bool returnClassForAOT)
   {
   // if method is unresolved, return right away
   if (cpIndex < 0)
      return nullptr;

   TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
      {
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor()); 
      auto &classOfStaticCache = getJ9ClassInfo(compInfoPT, _ramClass)._classOfStaticCache;
      if (!classOfStaticCache)
         {
         // initialize cache, called once per ram class
         classOfStaticCache = new (PERSISTENT_NEW) PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *>(PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *>::allocator_type(TR::Compiler->persistentAllocator()));
         }
      
      auto it = classOfStaticCache->find(cpIndex);
      if (it != classOfStaticCache->end())
         return it->second;
      }

   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_classOfStatic, getRemoteMirror(), cpIndex, returnClassForAOT);
   TR_OpaqueClassBlock *classOfStatic = std::get<0>(_stream->read<TR_OpaqueClassBlock *>());
   if (classOfStatic)
      {
      // reacquire monitor and cache, if client returned a valid class
      // if client returned NULL, don't cache, because class might not be fully initialized,
      // so the result may change in the future
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor()); 
      auto classOfStaticCache = getJ9ClassInfo(compInfoPT, _ramClass)._classOfStaticCache;
      classOfStaticCache->insert({cpIndex, classOfStatic});
      }
   return classOfStatic;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isConstantDynamic(I_32 cpIndex)
   {
   TR_ASSERT_FATAL(cpIndex != -1, "ConstantDynamic cpIndex shouldn't be -1");
   UDATA cpType = J9_CP_TYPE(J9ROMCLASS_CPSHAPEDESCRIPTION(_romClass), cpIndex);
   return (J9CPTYPE_CONSTANT_DYNAMIC == cpType);
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isUnresolvedString(I_32 cpIndex, bool optimizeForAOT)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isUnresolvedString, getRemoteMirror(), cpIndex, optimizeForAOT);
   return std::get<0>(_stream->read<bool>());
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedPossiblyPrivateVirtualMethod(TR::Compilation * comp, I_32 cpIndex, bool ignoreRtResolve, bool * unresolvedInCP)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
#if TURN_OFF_INLINING
   return 0;
#else
   TR_ResolvedMethod *resolvedMethod = nullptr;
   bool cached = getCachedResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::VirtualFromCP, cpIndex), &resolvedMethod, unresolvedInCP);

   if (!cached)
      {
      // See if the constant pool entry is already resolved or not
      if (unresolvedInCP)
          *unresolvedInCP = true;

      if (!((_fe->_jitConfig->runtimeFlags & J9JIT_RUNTIME_RESOLVE) &&
            !comp->ilGenRequest().details().isMethodHandleThunk() && // cmvc 195373
            performTransformation(comp, "Setting as unresolved virtual call cpIndex=%d\n",cpIndex) ) || ignoreRtResolve)
         {
         _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedPossiblyPrivateVirtualMethodAndMirror, getRemoteMirror(), literals(), cpIndex, getResolvedMethodParams());
         auto recv = _stream->read<J9Method *, UDATA, bool, TR_ResolvedJ9JITaaSServerMethodInfo, TR_ResolvedJ9Method *>();
         J9Method *ramMethod = std::get<0>(recv);
         UDATA vTableIndex = std::get<1>(recv);

         if (std::get<2>(recv) && unresolvedInCP)
            *unresolvedInCP = false;

         if (!_remoteMirror)
            {
            _remoteMirror = std::get<4>(recv);
            TR_ASSERT(_remoteMirror, "_remoteMirror must be initialized by now");
            }

         bool createResolvedMethod = true;

         if (comp->compileRelocatableCode() && ramMethod && comp->getOption(TR_UseSymbolValidationManager))
            {
            if (!comp->getSymbolValidationManager()->addVirtualMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex))
               createResolvedMethod = false;
            }

         if (vTableIndex)
            {
            TR_AOTInliningStats *aotStats = nullptr;
            if (comp->getOption(TR_EnableAOTStats))
               aotStats = & (((TR_JitPrivateConfig *)_fe->_jitConfig->privateConfig)->aotStats->virtualMethods);

            TR_ResolvedJ9JITaaSServerMethodInfo methodInfo = std::get<3>(recv);
            
            // call constructor without making a new query
            if (createResolvedMethod)
               {
               resolvedMethod = createResolvedMethodFromJ9Method(comp, cpIndex, vTableIndex, ramMethod, unresolvedInCP, aotStats, methodInfo);
               cacheResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::VirtualFromCP, cpIndex), (TR_OpaqueMethodBlock *) ramMethod, (uint32_t) vTableIndex, resolvedMethod, methodInfo);
               }
            }
         }
      }

   if (resolvedMethod == nullptr)
      {
      TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/virtual/null");
      if (unresolvedInCP)
         handleUnresolvedVirtualMethodInCP(cpIndex, unresolvedInCP);
      }
   else
      {
      if (((TR_ResolvedJ9Method*)resolvedMethod)->isVarHandleAccessMethod())
         {
         // VarHandle access methods are resolved to *_impl()V, restore their signatures to obtain function correctness in the Walker
         J9ROMConstantPoolItem *cpItem = (J9ROMConstantPoolItem *)romLiterals();
         J9ROMMethodRef *romMethodRef = (J9ROMMethodRef *)(cpItem + cpIndex);
         J9ROMNameAndSignature *nameAndSig = J9ROMFIELDREF_NAMEANDSIGNATURE(romMethodRef);
         int32_t signatureLength;
         char   *signature = utf8Data(J9ROMNAMEANDSIGNATURE_SIGNATURE(nameAndSig), signatureLength);
         ((TR_ResolvedJ9Method *)resolvedMethod)->setSignature(signature, signatureLength, comp->trMemory());
         }

      TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/virtual");
      TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/virtual:#bytes", sizeof(TR_ResolvedJ9Method));
      
      }

   return resolvedMethod;
#endif
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedVirtualMethod(
   TR::Compilation *comp,
   I_32 cpIndex,
   bool ignoreRtResolve,
   bool *unresolvedInCP)
   {
   TR_ResolvedMethod *method = getResolvedPossiblyPrivateVirtualMethod(
      comp,
      cpIndex,
      ignoreRtResolve,
      unresolvedInCP);

   return (method == NULL || method->isPrivate()) ? NULL : method;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::fieldsAreSame(int32_t cpIndex1, TR_ResolvedMethod *m2, int32_t cpIndex2, bool &sigSame)
   {
   if (TR::comp()->compileRelocatableCode())
      // in AOT, this should always return false, because mainline compares class loaders
      // with fe->sameClassLoaders, which always returns false for AOT compiles
      return false;
   TR_ResolvedJ9JITaaSServerMethod *serverMethod2 = static_cast<TR_ResolvedJ9JITaaSServerMethod*>(m2);
   if (getClassLoader() != serverMethod2->getClassLoader())
      return false;

   if (cpIndex1 == -1 || cpIndex2 == -1)
      return false;

   if (cpIndex1 == cpIndex2 && ramMethod() == serverMethod2->ramMethod())
      return true;
   
   int32_t sig1Len = 0, sig2Len = 0;
   char *signature1 = fieldOrStaticSignatureChars(cpIndex1, sig1Len);
   char *signature2 = serverMethod2->fieldOrStaticSignatureChars(cpIndex2, sig2Len);
   
   int32_t name1Len = 0, name2Len = 0;
   char *name1 = fieldOrStaticNameChars(cpIndex1, name1Len);
   char *name2 = serverMethod2->fieldOrStaticNameChars(cpIndex2, name2Len);

   if (sig1Len == sig2Len && !memcmp(signature1, signature2, sig1Len) &&
       name1Len == name2Len && !memcmp(name1, name2, name1Len))
      {
      int32_t class1Len = 0, class2Len = 0;
      char *declaringClassName1 = classNameOfFieldOrStatic(cpIndex1, class1Len);
      char *declaringClassName2 = serverMethod2->classNameOfFieldOrStatic(cpIndex2, class2Len);

      if (class1Len == class2Len && !memcmp(declaringClassName1, declaringClassName2, class1Len))
          return true;
      }
   else
      {
      sigSame = false;
      }
   return false;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::staticsAreSame(int32_t cpIndex1, TR_ResolvedMethod *m2, int32_t cpIndex2, bool &sigSame)
   {
   if (TR::comp()->compileRelocatableCode())
      // in AOT, this should always return false, because mainline compares class loaders
      // with fe->sameClassLoaders, which always returns false for AOT compiles
      return false;
   TR_ResolvedJ9JITaaSServerMethod *serverMethod2 = static_cast<TR_ResolvedJ9JITaaSServerMethod*>(m2);
   if (getClassLoader() != serverMethod2->getClassLoader())
      return false;

   if (cpIndex1 == -1 || cpIndex2 == -1)
      return false;

   if (cpIndex1 == cpIndex2 && ramMethod() == serverMethod2->ramMethod())
      return true;
   
   // the client version of this method compares the addresses of values first, which we cannot do on the server. 
   // skipping that step affects performance, but does not affect correctness. 
   int32_t sig1Len = 0, sig2Len = 0;
   char *signature1 = fieldOrStaticSignatureChars(cpIndex1, sig1Len);
   char *signature2 = serverMethod2->fieldOrStaticSignatureChars(cpIndex2, sig2Len);
   
   int32_t name1Len = 0, name2Len = 0;
   char *name1 = fieldOrStaticNameChars(cpIndex1, name1Len);
   char *name2 = serverMethod2->fieldOrStaticNameChars(cpIndex2, name2Len);

   if (sig1Len == sig2Len && !memcmp(signature1, signature2, sig1Len) &&
       name1Len == name2Len && !memcmp(name1, name2, name1Len))
      {
      int32_t class1Len = 0, class2Len = 0;
      char *declaringClassName1 = classNameOfFieldOrStatic(cpIndex1, class1Len);
      char *declaringClassName2 = serverMethod2->classNameOfFieldOrStatic(cpIndex2, class2Len);

      if (class1Len == class2Len && !memcmp(declaringClassName1, declaringClassName2, class1Len))
          return true;
      }
   else
      {
      sigSame = false;
      }
   return false;
   }

TR_FieldAttributesCache *
TR_ResolvedJ9JITaaSServerMethod::getAttributesCache(bool isStatic, bool unresolvedInCP)
   {
   TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
   auto &attributesCache = isStatic ? 
      getJ9ClassInfo(compInfoPT, _ramClass)._staticAttributesCache :
      getJ9ClassInfo(compInfoPT, _ramClass)._fieldAttributesCache;
   if (!attributesCache)
      {
      // initialize cache, called once per ram class
      attributesCache = new (PERSISTENT_NEW) TR_FieldAttributesCache(TR_FieldAttributesCache::allocator_type(TR::Compiler->persistentAllocator()));
      }
   return attributesCache;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::getCachedFieldAttributes(int32_t cpIndex, TR_J9MethodFieldAttributes &attributes, bool isStatic)
   {
      {
      // First, search a global cache
      TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor()); 
      auto attributesCache = getAttributesCache(isStatic);
      auto it = attributesCache->find(cpIndex);
      if (it != attributesCache->end())
         {
         attributes = it->second;
         return true;
         }
      }

   // If global cache is empty, search local cache
   // Local cache is searched after global, because it only stores
   // unresolved field attributes, so most attributes should be stored globally
   auto &localCache = isStatic ? _staticAttributesCache : _fieldAttributesCache;
   auto it = localCache.find(cpIndex);
   if (it != localCache.end())
      {
      attributes = it->second;
      return true;
      }
   return false;
   }

void
TR_ResolvedJ9JITaaSServerMethod::cacheFieldAttributes(int32_t cpIndex, const TR_J9MethodFieldAttributes &attributes, bool isStatic)
   {
   TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
   if (attributes.isUnresolvedInCP())
      {
      // field is unresolved in CP, can later become resolved, can only be cached per resolved method.
      auto &attributesCache = isStatic ? _staticAttributesCache : _fieldAttributesCache;
      attributesCache.insert({cpIndex, attributes});
      }
   else
      {
      // field is resolved in CP, can cache globally per RAM class.
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor()); 
      auto attributesCache = getAttributesCache(isStatic);
#if defined(DEBUG) || defined(PROD_WITH_ASSUMES)
      TR_ASSERT(canCacheFieldAttributes(cpIndex, attributes, isStatic), "new and cached field attributes are not equal");
#endif
      attributesCache->insert({cpIndex, attributes});
      }
   }

bool
TR_ResolvedJ9JITaaSServerMethod::canCacheFieldAttributes(int32_t cpIndex, const TR_J9MethodFieldAttributes &attributes, bool isStatic)
   {
   auto attributesCache = getAttributesCache(isStatic);
   auto it = attributesCache->find(cpIndex);
   if (it != attributesCache->end())
      {
      // Attempting to cache attributes when this key is already cached.
      // This case can happen when two threads call `getCachedFieldAttributes`,
      // see that attributes are not cached, and they both make a remote call
      // and call this method.
      // Make sure that attributes they are caching are the same.
      auto cachedAttrs = it->second;
      return attributes == cachedAttrs;
      }
   return true;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::fieldAttributes(TR::Compilation * comp, I_32 cpIndex, U_32 * fieldOffset, TR::DataType * type, bool * volatileP, bool * isFinal, bool * isPrivate, bool isStore, bool * unresolvedInCP, bool needAOTValidation)
   {
   bool isStatic = false;
   TR_J9MethodFieldAttributes attributes;
   if(!getCachedFieldAttributes(cpIndex, attributes, isStatic))
      {
      // make a remote call, if attributes are not cached
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_fieldAttributes, getRemoteMirror(), cpIndex, isStore, needAOTValidation, getResolvedMethodParams());
      auto recv = _stream->read<TR_J9MethodFieldAttributes, TR_ResolvedJ9Method *>();
      attributes = std::get<0>(recv);

      cacheFieldAttributes(cpIndex, attributes, isStatic);
      if (!_remoteMirror)
         _remoteMirror = std::get<1>(recv);
      }
   else
      {
#if defined(DEBUG) || defined(PROD_WITH_ASSUMES)
      bool validated = validateMethodFieldAttributes(attributes, isStatic, cpIndex, isStore, needAOTValidation);
      TR_ASSERT(validated, "field attributes from client and cache do not match");
#endif
      }
   
   bool result;
   attributes.setMethodFieldAttributesResult(fieldOffset, type, volatileP, isFinal, isPrivate, unresolvedInCP, &result);
   return result;
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedStaticMethod(TR::Compilation * comp, I_32 cpIndex, bool * unresolvedInCP)
   {
   // JITaaS TODO: Decide whether the counters should be updated on the server or the client
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");

   TR_ResolvedMethod *resolvedMethod = nullptr;
   if (getCachedResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Static, cpIndex), &resolvedMethod, unresolvedInCP))
      return resolvedMethod;

   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedStaticMethodAndMirror, getRemoteMirror(), cpIndex, getResolvedMethodParams());
   auto recv = _stream->read<J9Method *, TR_ResolvedJ9JITaaSServerMethodInfo, bool, TR_ResolvedJ9Method *>();
   J9Method * ramMethod = std::get<0>(recv); 
   if (unresolvedInCP)
      *unresolvedInCP = std::get<2>(recv);
   if (!_remoteMirror)
      {
      _remoteMirror = (TR_ResolvedJ9Method *) std::get<3>(recv);
      TR_ASSERT(_remoteMirror, "_remoteMirror must be initialized by now");
      }

   if (comp->compileRelocatableCode() && comp->getOption(TR_UseSymbolValidationManager) && ramMethod)
      {
      if (!comp->getSymbolValidationManager()->addStaticMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex))
         ramMethod = NULL;
      }

   bool skipForDebugging = false;
   if (isArchetypeSpecimen())
      {
      // ILGen macros currently must be resolved for correctness, or else they
      // are not recognized and expanded.  If we have unresolved calls, we can't
      // tell whether they're ilgen macros because the recognized-method system
      // only works on resovled methods.
      //
      if (ramMethod)
         skipForDebugging = false;
      else
         {
         comp->failCompilation<TR::ILGenFailure>("Can't compile an archetype specimen with unresolved calls");
         }
      }

   if (ramMethod && !skipForDebugging)
      {
      auto methodInfo = std::get<1>(recv);
      TR_AOTInliningStats *aotStats = NULL;
      if (comp->getOption(TR_EnableAOTStats))
         aotStats = & (((TR_JitPrivateConfig *)_fe->_jitConfig->privateConfig)->aotStats->staticMethods);
      resolvedMethod = createResolvedMethodFromJ9Method(comp, cpIndex, 0, ramMethod, unresolvedInCP, aotStats, methodInfo);
      cacheResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Static, cpIndex), (TR_OpaqueMethodBlock *) ramMethod, 0, resolvedMethod, methodInfo);
      if (unresolvedInCP)
         *unresolvedInCP = false;
      }

   if (resolvedMethod == NULL)
      {
      if (unresolvedInCP)
         handleUnresolvedStaticMethodInCP(cpIndex, unresolvedInCP);
      }

   return resolvedMethod;
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedSpecialMethod(TR::Compilation * comp, I_32 cpIndex, bool * unresolvedInCP)
   {
   // JITaaS TODO: Decide whether the counters should be updated on the server or the client
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
   
   TR_ResolvedMethod *resolvedMethod = nullptr;
   if (getCachedResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Special, cpIndex), &resolvedMethod, unresolvedInCP)) 
      return resolvedMethod;

   if (unresolvedInCP)
      *unresolvedInCP = true;

   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedSpecialMethodAndMirror, getRemoteMirror(), cpIndex, unresolvedInCP != nullptr, getResolvedMethodParams());
   auto recv = _stream->read<J9Method *, bool, bool, TR_ResolvedJ9JITaaSServerMethodInfo, TR_ResolvedJ9Method *>();
   J9Method * ramMethod = std::get<0>(recv);
   bool unresolved = std::get<1>(recv);
   bool tookBranch = std::get<2>(recv);

   if (unresolved && unresolvedInCP)
      *unresolvedInCP = true;

   if (!_remoteMirror)
      {
      _remoteMirror = (TR_ResolvedJ9Method *) std::get<4>(recv);
      TR_ASSERT(_remoteMirror, "_remoteMirror must be initialized by now");
      }

   if (tookBranch && ramMethod)
      {
      bool createResolvedMethod = true;
      if (comp->getOption(TR_UseSymbolValidationManager))
         {
         if (!comp->getSymbolValidationManager()->addSpecialMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex))
            createResolvedMethod = false;
         }
      auto methodInfo = std::get<3>(recv);
      TR_AOTInliningStats *aotStats = NULL;
      if (comp->getOption(TR_EnableAOTStats))
         aotStats = & (((TR_JitPrivateConfig *)_fe->_jitConfig->privateConfig)->aotStats->specialMethods);
      if (createResolvedMethod)
         {
         resolvedMethod = createResolvedMethodFromJ9Method(comp, cpIndex, 0, ramMethod, unresolvedInCP, aotStats, methodInfo);
         cacheResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Special, cpIndex), (TR_OpaqueMethodBlock *) ramMethod, 0, resolvedMethod, methodInfo);
         }
      if (unresolvedInCP)
         *unresolvedInCP = false;
      }

   if (resolvedMethod == NULL)
      {
      if (unresolvedInCP)
         handleUnresolvedVirtualMethodInCP(cpIndex, unresolvedInCP);
      }
   return resolvedMethod;
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodFromJ9Method( TR::Compilation *comp, int32_t cpIndex, uint32_t vTableSlot, J9Method *j9Method, bool * unresolvedInCP, TR_AOTInliningStats *aotStats)
   {
   return new (comp->trHeapMemory()) TR_ResolvedJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) j9Method, _fe, comp->trMemory(), this, vTableSlot);
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodFromJ9Method( TR::Compilation *comp, int32_t cpIndex, uint32_t vTableSlot, J9Method *j9Method, bool * unresolvedInCP, TR_AOTInliningStats *aotStats, const TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo)
   {
   return new (comp->trHeapMemory()) TR_ResolvedJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) j9Method, _fe, comp->trMemory(), methodInfo, this, vTableSlot);
   }

uint32_t
TR_ResolvedJ9JITaaSServerMethod::classCPIndexOfMethod(uint32_t methodCPIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_classCPIndexOfMethod, getRemoteMirror(), methodCPIndex);
   return std::get<0>(_stream->read<uint32_t>());
   }

void *
TR_ResolvedJ9JITaaSServerMethod::startAddressForJittedMethod()
   {
   // return the cached value if we have any
   if (_startAddressForJittedMethod)
      {
      return _startAddressForJittedMethod;
      }
   else // Otherwise ask the client for it
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_startAddressForJittedMethod, getRemoteMirror());
      return std::get<0>(_stream->read<void *>());
      }
   }

char *
TR_ResolvedJ9JITaaSServerMethod::localName(U_32 slotNumber, U_32 bcIndex, I_32 &len, TR_Memory *trMemory)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_localName, getRemoteMirror(), slotNumber, bcIndex);
   const std::string nameString = std::get<0>(_stream->read<std::string>());
   len = nameString.length();
   char *out = (char*) trMemory->allocateHeapMemory(len);
   memcpy(out, &nameString[0], len);
   return out;
   }

TR_OpaqueClassBlock *
TR_ResolvedJ9JITaaSServerMethod::getResolvedInterfaceMethod(I_32 cpIndex, UDATA *pITableIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedInterfaceMethod_2, getRemoteMirror(), cpIndex);
   auto recv = _stream->read<TR_OpaqueClassBlock *, UDATA>();
   *pITableIndex = std::get<1>(recv);
   auto result = std::get<0>(recv);

   auto comp = _fe->_compInfoPT->getCompilation();
   if (comp && comp->compileRelocatableCode() && comp->getOption(TR_UseSymbolValidationManager))
      {
      if (!comp->getSymbolValidationManager()->addClassFromITableIndexCPRecord(result, cp(), cpIndex))
         result = NULL;
      }
   return result;
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedInterfaceMethod(TR::Compilation * comp, TR_OpaqueClassBlock * classObject, I_32 cpIndex)
   {
   TR_ResolvedMethod *resolvedMethod = nullptr;
   if (getCachedResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Interface, cpIndex, classObject), &resolvedMethod, nullptr))
      return resolvedMethod;
   
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedInterfaceMethodAndMirror_3, getPersistentIdentifier(), classObject, cpIndex, getRemoteMirror(), getResolvedMethodParams());
   auto recv = _stream->read<bool, J9Method*, TR_ResolvedJ9JITaaSServerMethodInfo, TR_ResolvedJ9Method *>();
   bool resolved = std::get<0>(recv);
   J9Method *ramMethod = std::get<1>(recv);
   auto &methodInfo = std::get<2>(recv);
   
   if (!_remoteMirror)
      _remoteMirror = std::get<3>(recv);

   if (comp && comp->getOption(TR_UseSymbolValidationManager))
      {
      if (!comp->getSymbolValidationManager()->addInterfaceMethodFromCPRecord((TR_OpaqueMethodBlock *) ramMethod,
                                                                              (TR_OpaqueClassBlock *) ((TR_J9VM *) _fe)->getClassFromMethodBlock(getPersistentIdentifier()),
                                                                              classObject,
                                                                              cpIndex))
         {
         return NULL;
         }
      }


   // If the method ref is unresolved, the bytecodes of the ramMethod will be NULL.
   // IFF resolved, then we can look at the rest of the ref.
   //
   if (resolved)
      {
      TR_AOTInliningStats *aotStats = NULL;
      if (comp->getOption(TR_EnableAOTStats))
         aotStats = & (((TR_JitPrivateConfig *)_fe->_jitConfig->privateConfig)->aotStats->interfaceMethods);
      TR_ResolvedMethod *m = createResolvedMethodFromJ9Method(comp, cpIndex, 0, ramMethod, NULL, aotStats, methodInfo);

      TR_OpaqueClassBlock *c = NULL;
      if (m)
         {
         c = m->classOfMethod();
         if (c && !_fe->isInterfaceClass(c))
            {
            TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/interface");
            TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/interface:#bytes", sizeof(TR_ResolvedJ9Method));
            resolvedMethod = m;
            // for resolved interface method, need to know cpIndex, as well as classObject
            // to uniquely identify it.
            cacheResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::Interface, cpIndex, classObject), (TR_OpaqueMethodBlock *) ramMethod, 0, resolvedMethod, methodInfo);
            }
         }
      }
   if (resolvedMethod)
      return resolvedMethod;

   TR::DebugCounter::incStaticDebugCounter(comp, "resources.resolvedMethods/interface/null");
   return 0;
   }

U_32
TR_ResolvedJ9JITaaSServerMethod::getResolvedInterfaceMethodOffset(TR_OpaqueClassBlock * classObject, I_32 cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedInterfaceMethodOffset, getRemoteMirror(), classObject, cpIndex);
   auto recv = _stream->read<U_32>();
   return std::get<0>(recv);
   }

/* Only returns non-null if the method is not to be dispatched by itable, i.e.
 * if it is:
 * - private (isPrivate()), using direct dispatch;
 * - a final method of Object (isFinalInObject()), using direct dispatch; or
 * - a non-final method of Object, using virtual dispatch.
 */
TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedImproperInterfaceMethod(TR::Compilation * comp, I_32 cpIndex)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
#if TURN_OFF_INLINING
   return 0;
#else
   if ((_fe->_jitConfig->runtimeFlags & J9JIT_RUNTIME_RESOLVE) == 0)
      {
      // query for resolved method and create its mirror at the same time
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedImproperInterfaceMethodAndMirror, getRemoteMirror(), cpIndex);
      auto recv = _stream->read<J9Method *, TR_ResolvedJ9JITaaSServerMethodInfo>();
      auto j9method = std::get<0>(recv);
      auto methodInfo = std::get<1>(recv);

      if (comp->getOption(TR_UseSymbolValidationManager) && j9method)
         {
         if (!comp->getSymbolValidationManager()->addImproperInterfaceMethodFromCPRecord((TR_OpaqueMethodBlock *)j9method, cp(), cpIndex))
            j9method = NULL;
         }

      if (j9method == NULL)
         return NULL;
      else
         return createResolvedMethodFromJ9Method(comp, cpIndex, 0, j9method, NULL, NULL, methodInfo);
      }

   return NULL;
#endif
   }

void *
TR_ResolvedJ9JITaaSServerMethod::startAddressForJNIMethod(TR::Compilation *comp)
   {
   // For fastJNI methods, we have the address cached
   if (_jniProperties)
      return _jniTargetAddress;
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_startAddressForJNIMethod, getRemoteMirror());
   return std::get<0>(_stream->read<void *>());
   }

void *
TR_ResolvedJ9JITaaSServerMethod::startAddressForInterpreterOfJittedMethod()
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_startAddressForInterpreterOfJittedMethod, getRemoteMirror());
   return std::get<0>(_stream->read<void *>());
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedVirtualMethod(TR::Compilation * comp, TR_OpaqueClassBlock * classObject, I_32 virtualCallOffset, bool ignoreRtResolve)
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *)_fe;
   if (fej9->_jitConfig->runtimeFlags & J9JIT_RUNTIME_RESOLVE && !ignoreRtResolve)
      return NULL;

   TR_ResolvedMethod *resolvedMethod = NULL;
   // If method is already cached, return right away
   if (getCachedResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::VirtualFromOffset, virtualCallOffset, classObject), &resolvedMethod)) 
      return resolvedMethod;

   // Remote call finds RAM method at offset and creates a resolved method at the client
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedVirtualMethod, classObject, virtualCallOffset, ignoreRtResolve, _remoteMirror);
   auto recv = _stream->read<TR_OpaqueMethodBlock *, TR_ResolvedJ9JITaaSServerMethodInfo>();
   auto ramMethod = std::get<0>(recv);
   auto &methodInfo = std::get<1>(recv);

   // it seems better to override this method for AOT but that's what baseline is doing
   // maybe there is a reason for it
   if (_fe->isAOT_DEPRECATED_DO_NOT_USE())
      {
      if (comp && comp->getOption(TR_UseSymbolValidationManager))
         {
         if (!comp->getSymbolValidationManager()->addVirtualMethodFromOffsetRecord(ramMethod, classObject, virtualCallOffset, ignoreRtResolve))
            return NULL;
         }
      resolvedMethod = ramMethod ? new (comp->trHeapMemory()) TR_ResolvedRelocatableJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) ramMethod, _fe, comp->trMemory(), methodInfo, this) : 0;
      }
   else
      {
      resolvedMethod = ramMethod ? new (comp->trHeapMemory()) TR_ResolvedJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) ramMethod, _fe, comp->trMemory(), methodInfo, this) : 0;
      }
   cacheResolvedMethod(getResolvedMethodKey(TR_ResolvedMethodType::VirtualFromOffset, virtualCallOffset, classObject), ramMethod, 0, resolvedMethod, methodInfo);
   return resolvedMethod;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::inROMClass(void * address)
   {
   return address >= romClassPtr() && address < ((uint8_t*)romClassPtr()) + romClassPtr()->romSize;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::getRemoteROMString(int32_t &len, void *basePtr, std::initializer_list<size_t> offsets)
   {
   auto offsetFirst = *offsets.begin();
   auto offsetSecond = (offsets.size() == 2) ? *(offsets.begin() + 1) : 0;

   TR_ASSERT(offsetFirst < (1 << 16) && offsetSecond < (1 << 16), "Offsets are larger than 16 bits");
   TR_ASSERT(offsets.size() <= 2, "Number of offsets is greater than 2"); 

   // create a key for hashing into a table of strings
   TR_RemoteROMStringKey key;
   uint32_t offsetKey = (offsetFirst << 16) + offsetSecond;
   key.basePtr = basePtr;
   key.offsets = offsetKey;
   
   std::string *cachedStr = nullptr;
   bool isCached = false;
   TR::CompilationInfoPerThread *threadCompInfo = _fe->_compInfoPT;
   {
      OMR::CriticalSection getRemoteROMClass(threadCompInfo->getClientData()->getROMMapMonitor()); 
      auto &stringsCache = getJ9ClassInfo(threadCompInfo, _ramClass)._remoteROMStringsCache;
      // initialize cache if it hasn't been done yet
      if (!stringsCache)
         {
         stringsCache = new (PERSISTENT_NEW) PersistentUnorderedMap<TR_RemoteROMStringKey, std::string>(PersistentUnorderedMap<TR_RemoteROMStringKey, std::string>::allocator_type(TR::Compiler->persistentAllocator()));
         }
      auto gotStr = stringsCache->find(key);
      if (gotStr != stringsCache->end())
         {
         cachedStr = &(gotStr->second);
         isCached = true;
         }
   }

   // only make a query if a string hasn't been cached
   if (!isCached)
      {
      size_t offsetFromROMClass = (uint8_t*) basePtr - (uint8_t*) romClassPtr();
      std::string offsetsStr((char*) offsets.begin(), offsets.size() * sizeof(size_t));
      
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getRemoteROMString, getRemoteMirror(), offsetFromROMClass, offsetsStr);
         {
         // reaquire the monitor
         OMR::CriticalSection getRemoteROMClass(threadCompInfo->getClientData()->getROMMapMonitor()); 
         auto &stringsCache = getJ9ClassInfo(threadCompInfo, _ramClass)._remoteROMStringsCache;
         cachedStr = &(stringsCache->insert({key, std::get<0>(_stream->read<std::string>())}).first->second);
         }
      }

   len = cachedStr->length();
   return &(cachedStr->at(0));
   }

// Takes a pointer to some data which is placed statically relative to the rom class,
// as well as a list of offsets to J9SRP fields. The first offset is applied before the first
// SRP is followed.
//
// If at any point while following the chain of SRP pointers we land outside the ROM class,
// then we fall back to getRemoteROMString which follows the same process on the client.
//
// This is a workaround because some data referenced in the ROM constant pool is located outside of
// it, but we cannot easily determine if the reference is outside or not (or even if it is a reference!)
// because the data is untyped.
char *
TR_ResolvedJ9JITaaSServerMethod::getROMString(int32_t &len, void *basePtr, std::initializer_list<size_t> offsets)
   {
   uint8_t *ptr = (uint8_t*) basePtr;
   for (size_t offset : offsets)
      {
      ptr += offset;
      if (!inROMClass(ptr))
         return getRemoteROMString(len, basePtr, offsets);
      ptr = ptr + *(J9SRP*)ptr;
      }
   if (!inROMClass(ptr))
      return getRemoteROMString(len, basePtr, offsets);
   char *data = utf8Data((J9UTF8*) ptr, len);
   return data;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::fieldOrStaticSignatureChars(I_32 cpIndex, int32_t & len)
   {
   if (cpIndex < 0)
      return 0;

   char *signature = getROMString(len, &romCPBase()[cpIndex],
                           {
                           offsetof(J9ROMFieldRef, nameAndSignature),
                           offsetof(J9ROMNameAndSignature, signature)
                           });
   return signature;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::getClassNameFromConstantPool(uint32_t cpIndex, uint32_t &length)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");

   int32_t len;
   char *name = getROMString(len, &romLiterals()[cpIndex],
                           {
                           offsetof(J9ROMClassRef, name),
                           });
   length = len; // this is what happends when you have no type conventions
   return name;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::classNameOfFieldOrStatic(I_32 cpIndex, int32_t & len)
   {
   if (cpIndex == -1)
      return 0;

   J9ROMFieldRef * ref = (J9ROMFieldRef *) (&romCPBase()[cpIndex]);
   TR_ASSERT(inROMClass(ref), "field ref must be in ROM class"); // for now. if this fails make it a remote call
   char *name = getROMString(len, &romCPBase()[ref->classRefCPIndex],
                           {
                           offsetof(J9ROMClassRef, name),
                           });
   return name;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::classSignatureOfFieldOrStatic(I_32 cpIndex, int32_t & len)
   {
   if (cpIndex == -1)
      return 0;

   char *signature = getROMString(len, &romCPBase()[cpIndex],
                           {
                           offsetof(J9ROMFieldRef, nameAndSignature),
                           offsetof(J9ROMNameAndSignature, signature),
                           });
   return signature;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::fieldOrStaticNameChars(I_32 cpIndex, int32_t & len)
   {
   if (cpIndex < 0)
      return 0;

   char *name = getROMString(len, &romCPBase()[cpIndex],
                           {
                           offsetof(J9ROMFieldRef, nameAndSignature),
                           offsetof(J9ROMNameAndSignature, name)
                           });
   return name;
   }

char *
TR_ResolvedJ9JITaaSServerMethod::fieldOrStaticName(I_32 cpIndex, int32_t & len, TR_Memory * trMemory, TR_AllocationKind kind)
   {
   if (cpIndex == -1)
      return "<internal name>";

   J9ROMFieldRef * ref = (J9ROMFieldRef *) (&romCPBase()[cpIndex]);
   J9ROMNameAndSignature * nameAndSignature = J9ROMFIELDREF_NAMEANDSIGNATURE(ref);

   if (inROMClass(nameAndSignature))
      {
      J9UTF8 * declName = J9ROMCLASSREF_NAME((J9ROMClassRef *) (&romCPBase()[ref->classRefCPIndex]));
      J9UTF8 * name = J9ROMNAMEANDSIGNATURE_NAME(nameAndSignature);
      J9UTF8 * signature = J9ROMNAMEANDSIGNATURE_SIGNATURE(nameAndSignature);

      if (inROMClass(declName) && inROMClass(name) && inROMClass(signature))
         {
         len = J9UTF8_LENGTH(declName) + J9UTF8_LENGTH(name) + J9UTF8_LENGTH(signature) +3;
         char * s = (char *)trMemory->allocateMemory(len, kind);
         sprintf(s, "%.*s.%.*s %.*s",
                 J9UTF8_LENGTH(declName), utf8Data(declName),
                 J9UTF8_LENGTH(name), utf8Data(name),
                 J9UTF8_LENGTH(signature), utf8Data(signature));
         return s;
         }
      }

   
   std::string *cachedStr = nullptr;
   bool isCached = false;
   TR::CompilationInfoPerThread *threadCompInfo = _fe->_compInfoPT;
   {
      OMR::CriticalSection getRemoteROMClass(threadCompInfo->getClientData()->getROMMapMonitor()); 
      auto &stringsCache = getJ9ClassInfo(threadCompInfo, _ramClass)._fieldOrStaticNameCache;
      // initialize cache if it hasn't been done yet
      if (!stringsCache)
         {
         stringsCache = new (PERSISTENT_NEW) PersistentUnorderedMap<int32_t, std::string>(PersistentUnorderedMap<TR_RemoteROMStringKey, std::string>::allocator_type(TR::Compiler->persistentAllocator()));
         }
      auto gotStr = stringsCache->find(cpIndex);
      if (gotStr != stringsCache->end())
         {
         cachedStr = &(gotStr->second);
         isCached = true;
         }
   }

   // only make a query if a string hasn't been cached
   if (!isCached)
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_fieldOrStaticName, getRemoteMirror(), cpIndex);
         {
         // reaquire the monitor
         OMR::CriticalSection getRemoteROMClass(threadCompInfo->getClientData()->getROMMapMonitor()); 
         auto &stringsCache = getJ9ClassInfo(threadCompInfo, _ramClass)._fieldOrStaticNameCache;
         cachedStr = &(stringsCache->insert({cpIndex, std::get<0>(_stream->read<std::string>())}).first->second);
         }
      }

   len = cachedStr->length();
   return &(cachedStr->at(0));
   }

void *
TR_ResolvedJ9JITaaSServerMethod::stringConstant(I_32 cpIndex)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_stringConstant, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<void *>());
   }


bool
TR_ResolvedJ9JITaaSServerMethod::isSubjectToPhaseChange(TR::Compilation *comp)
   {
   bool candidate = comp->getOptLevel() <= warm &&
        // comp->getPersistentInfo()->getJitState() == STARTUP_STATE // This needs to be asked at the server
        isPublic() &&
        (
         strncmp("java/util/AbstractCollection", comp->signature(), 28) == 0 ||
         strncmp("java/util/Hash", comp->signature(), 14) == 0 ||
         strncmp("java/lang/String", comp->signature(), 16) == 0 ||
         strncmp("sun/nio/", comp->signature(), 8) == 0
         );
 
   if (!candidate)
      {
      return false;
      }
   else
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isSubjectToPhaseChange, getRemoteMirror());
      return std::get<0>(_stream->read<bool>());
      // JITaaS TODO: cache the JitState when we create  TR_ResolvedJ9JITaaSServerMethod
      // This may not behave exactly like the non-JITaaS due to timing differences
      }
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedHandleMethod(TR::Compilation * comp, I_32 cpIndex, bool * unresolvedInCP)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
#if TURN_OFF_INLINING
   return 0;
#else

   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedHandleMethod, getRemoteMirror(), cpIndex);
   auto recv = _stream->read<TR_OpaqueMethodBlock *, std::string, bool>();
   auto dummyInvokeExact = std::get<0>(recv);
   auto signature = std::get<1>(recv);
   if (unresolvedInCP)
      *unresolvedInCP = std::get<2>(recv);

   return _fe->createResolvedMethodWithSignature(comp->trMemory(), dummyInvokeExact, NULL, &signature[0], signature.length(), this);
#endif
   }

#if defined(J9VM_OPT_REMOVE_CONSTANT_POOL_SPLITTING)
void *
TR_ResolvedJ9JITaaSServerMethod::methodTypeTableEntryAddress(int32_t cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_methodTypeTableEntryAddress, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<void*>());
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isUnresolvedMethodTypeTableEntry(int32_t cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isUnresolvedMethodTypeTableEntry, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<bool>());
   }
#endif

bool
TR_ResolvedJ9JITaaSServerMethod::isUnresolvedCallSiteTableEntry(int32_t callSiteIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isUnresolvedCallSiteTableEntry, getRemoteMirror(), callSiteIndex);
   return std::get<0>(_stream->read<bool>());
   }

void *
TR_ResolvedJ9JITaaSServerMethod::callSiteTableEntryAddress(int32_t callSiteIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_callSiteTableEntryAddress, getRemoteMirror(), callSiteIndex);
   return std::get<0>(_stream->read<void*>());
   }

TR_ResolvedMethod *
TR_ResolvedJ9JITaaSServerMethod::getResolvedDynamicMethod(TR::Compilation * comp, I_32 callSiteIndex, bool * unresolvedInCP)
   {
   TR_ASSERT(callSiteIndex != -1, "callSiteIndex shouldn't be -1");

#if TURN_OFF_INLINING
   return 0;
#else

   J9Class    *ramClass = constantPoolHdr();
   J9ROMClass *romClass = romClassPtr();
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getResolvedDynamicMethod, callSiteIndex, ramClass);
   auto recv = _stream->read<TR_OpaqueMethodBlock*, std::string, bool>();
   TR_OpaqueMethodBlock *dummyInvokeExact = std::get<0>(recv);
   std::string signature = std::get<1>(recv);
   if (unresolvedInCP)
      *unresolvedInCP = std::get<2>(recv);
   TR_ResolvedMethod *result = _fe->createResolvedMethodWithSignature(comp->trMemory(), dummyInvokeExact, NULL, &signature[0], signature.size(), this);

   return result;
#endif
   }

bool
TR_ResolvedJ9JITaaSServerMethod::shouldFailSetRecognizedMethodInfoBecauseOfHCR()
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_shouldFailSetRecognizedMethodInfoBecauseOfHCR, getRemoteMirror());
   return std::get<0>(_stream->read<bool>());
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isSameMethod(TR_ResolvedMethod * m2)
   {
   if (isNative())
      return false; // A jitted JNI method doesn't call itself

   auto other = static_cast<TR_ResolvedJ9JITaaSServerMethod*>(m2);

   bool sameRamMethod = ramMethod() == other->ramMethod();
   if (!sameRamMethod)
      return false;

   if (asJ9Method()->isArchetypeSpecimen())
      {
      if (!other->asJ9Method()->isArchetypeSpecimen())
         return false;

      uintptrj_t *thisHandleLocation  = getMethodHandleLocation();
      uintptrj_t *otherHandleLocation = other->getMethodHandleLocation();

      // If these are not MethodHandle thunk archetypes, then we're not sure
      // how to compare them.  Conservatively return false in that case.
      //
      if (!thisHandleLocation)
         return false;
      if (!otherHandleLocation)
         return false;

      bool sameMethodHandle;

      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isSameMethod, thisHandleLocation, otherHandleLocation);
      sameMethodHandle = std::get<0>(_stream->read<bool>());

      if (sameMethodHandle)
         {
         // Same ramMethod, same handle.  This means we're talking about the
         // exact same thunk.
         //
         return true;
         }
      else
         {
         // Different method handle.  Assuming we're talking about a custom thunk,
         // then it will be different thunk.
         //
         return false;
         }
      }

   return true;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::isInlineable(TR::Compilation *comp)
   {
   // Reduce number of remote queries by testing the options first
   // This assumes knowledge of how the original is implemented
   if (comp->getOption(TR_FullSpeedDebug) && comp->getOption(TR_EnableOSR))
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_isInlineable, getRemoteMirror());
      return std::get<0>(_stream->read<bool>());
      }
   else
      {
      return true;
      }
   }

void
TR_ResolvedJ9JITaaSServerMethod::setWarmCallGraphTooBig(uint32_t bcIndex, TR::Compilation *comp)
   {
   // set the variable in the server IProfiler
   TR_ResolvedJ9Method::setWarmCallGraphTooBig(bcIndex, comp);
   // set it on the client IProfiler too, in case a server crashes, client will still have the correct value
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_setWarmCallGraphTooBig, getRemoteMirror(), bcIndex);
   _stream->read<JITaaS::Void>();
   }

void
TR_ResolvedJ9JITaaSServerMethod::setVirtualMethodIsOverridden()
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_setVirtualMethodIsOverridden, getRemoteMirror());
   _stream->read<JITaaS::Void>();
   }

bool
TR_ResolvedJ9JITaaSServerMethod::methodIsNotzAAPEligible()
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_methodIsNotzAAPEligible, getRemoteMirror());
   return std::get<0>(_stream->read<bool>());
   }
void
TR_ResolvedJ9JITaaSServerMethod::setClassForNewInstance(J9Class *c)
   {
   _j9classForNewInstance = c;
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_setClassForNewInstance, getRemoteMirror(), c);
   _stream->read<JITaaS::Void>();
   }

TR_OpaqueClassBlock *
TR_ResolvedJ9JITaaSServerMethod::classOfMethod()
      {
      if (isNewInstanceImplThunk())
         {
         TR_ASSERT(_j9classForNewInstance, "Must have the class for the newInstance");
         //J9Class * clazz = (J9Class *)((intptrj_t)ramMethod()->extra & ~J9_STARTPC_NOT_TRANSLATED);
         return _fe->convertClassPtrToClassOffset(_j9classForNewInstance);//(TR_OpaqueClassBlock *&)(rc);
         }
      return _fe->convertClassPtrToClassOffset(_ramClass);
      }

TR_PersistentJittedBodyInfo *
TR_ResolvedJ9JITaaSServerMethod::getExistingJittedBodyInfo()
   {
   return _bodyInfo; // return cached value
   }

void
TR_ResolvedJ9JITaaSServerMethod::getFaninInfo(uint32_t *count, uint32_t *weight, uint32_t *otherBucketWeight)
   {
   uint32_t i = 0;
   uint32_t w = 0;
   if (otherBucketWeight)
      *otherBucketWeight = 0;

   TR_IPMethodHashTableEntry *entry = _iProfilerMethodEntry;
   if (entry)
      {
      w = entry->_otherBucket.getWeight();
      // Iterate through all the callers and add their weight
      for (TR_IPMethodData* it = &entry->_caller; it; it = it->next)
         {
         w += it->getWeight();
         i++;
         }
      if (otherBucketWeight)
         *otherBucketWeight = entry->_otherBucket.getWeight();
      }
   *weight = w;
   *count = i;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::getCallerWeight(TR_ResolvedJ9Method *caller, uint32_t *weight, uint32_t pcIndex)
   {
   TR_OpaqueMethodBlock *callerMethod = caller->getPersistentIdentifier();
   bool useTuples = (pcIndex != ~0);

   //adjust pcIndex for interface calls (see getSearchPCFromMethodAndBCIndex)
   //otherwise we won't be able to locate a caller-callee-bcIndex triplet
   //even if it is in a TR_IPMethodHashTableEntry
   TR_IProfiler *iProfiler = _fe->getIProfiler();
   if (!iProfiler)
      return false;

   uintptrj_t pcAddress = iProfiler->getSearchPCFromMethodAndBCIndex(callerMethod, pcIndex, 0);

   TR_IPMethodHashTableEntry *entry = _iProfilerMethodEntry;

   if(!entry)   // if there are no entries, we have no callers!
      {
      *weight = ~0;
      return false;
      }
   for (TR_IPMethodData* it = &entry->_caller; it; it = it->next)
      {
      if( it->getMethod() == callerMethod && (!useTuples || ((((uintptrj_t) it->getPCIndex()) + TR::Compiler->mtd.bytecodeStart(callerMethod)) == pcAddress)))
         {
         *weight = it->getWeight();
         return true;
         }
      }

   *weight = entry->_otherBucket.getWeight();
   return false;
   }

void
TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodMirror(TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo, TR_OpaqueMethodBlock *method, uint32_t vTableSlot, TR_ResolvedMethod *owningMethod, TR_FrontEnd *fe, TR_Memory *trMemory)
   {
   // Create resolved method mirror on the client.
   // Should be called to mirror a call to resolved method constructor,
   // will work when method doesn't have an owning method (compilee method).
   // Resolved method should not be NULL
   TR_ResolvedJ9Method *resolvedMethod = NULL;
   if (!((TR_J9VMBase *) fe)->isAOT_DEPRECATED_DO_NOT_USE())
      resolvedMethod = new (trMemory->trHeapMemory()) TR_ResolvedJ9Method(method, fe, trMemory, owningMethod, vTableSlot);
   else
      resolvedMethod = new (trMemory->trHeapMemory()) TR_ResolvedRelocatableJ9Method(method, fe, trMemory, owningMethod, vTableSlot); 
   if (!resolvedMethod) throw std::bad_alloc();

   packMethodInfo(methodInfo, resolvedMethod, fe);
   }

void
TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodFromJ9MethodMirror(TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo, TR_OpaqueMethodBlock *method, uint32_t vTableSlot, TR_ResolvedMethod *owningMethod, TR_FrontEnd *fe, TR_Memory *trMemory)
   {
   // Create resolved method mirror on the client.
   // Should be called to mirror a call to TR_Resolved(Relocatable)MethodFromJ9Method::createResolvedMethodFromJ9Method.
   // Requires owning method to exist to work properly.
   // Resolved method can be NULL.
   TR_ASSERT(owningMethod, "owning method cannot be NULL");
   TR_ResolvedJ9Method *resolvedMethod = NULL;
   // The simplest solution would be to call owningMethod->createResolvedMethodFromJ9Method(...) here. 
   // Thanks to polymorphism, the correct version of the resolved method would be initialized and everything would be simple.
   // However, createResolvedMethodFromJ9Method is declared protected in TR_ResolvedJ9Method, so we can't call it here.
   // Maybe we should make it public, but that would require changing baseline.
   // For now, I'll have to do with mostly copy-pasting code from that method.
   TR_J9VMBase *fej9 = (TR_J9VMBase *) fe;
   if (!fej9->isAOT_DEPRECATED_DO_NOT_USE())
      {
      resolvedMethod = new (trMemory->trHeapMemory()) TR_ResolvedJ9Method(method, fe, trMemory, owningMethod, vTableSlot);
      if (!resolvedMethod) throw std::bad_alloc();
      }
   else
      {
      // Do the same thing createResolvedMethodFromJ9Method does, but without collecting stats
      TR::Compilation *comp = TR::comp();
#if defined(J9VM_OPT_SHARED_CLASSES) && (defined(TR_HOST_X86) || defined(TR_HOST_POWER) || defined(TR_HOST_S390) || defined(TR_HOST_ARM))
      bool resolveAOTMethods = !comp->getOption(TR_DisableAOTResolveDiffCLMethods);
      bool enableAggressive = comp->getOption(TR_EnableAOTInlineSystemMethod);
      bool isSystemClassLoader = false;
      J9Method *j9method = (J9Method *) method;

      if (comp->getOption(TR_DisableDFP) ||
          (!(TR::Compiler->target.cpu.supportsDecimalFloatingPoint()
#ifdef TR_TARGET_S390
          || TR::Compiler->target.cpu.getS390SupportsDFP()
#endif
            ) ||
             !TR_J9MethodBase::isBigDecimalMethod(j9method)))
         {
         // Check if same classloader
         J9Class *j9clazz = (J9Class *) J9_CLASS_FROM_CP(((J9RAMConstantPoolItem *) J9_CP_FROM_METHOD(((J9Method *)j9method))));
         TR_OpaqueClassBlock *clazzOfInlinedMethod = fej9->convertClassPtrToClassOffset(j9clazz);
         TR_OpaqueClassBlock *clazzOfCompiledMethod = fej9->convertClassPtrToClassOffset(J9_CLASS_FROM_METHOD(((TR_ResolvedJ9Method *) owningMethod)->ramMethod()));

         if (enableAggressive)
            {
            isSystemClassLoader = ((void*)fej9->vmThread()->javaVM->systemClassLoader->classLoaderObject ==  (void*)fej9->getClassLoader(clazzOfInlinedMethod));
            }

         if (TR::CompilationInfo::get(fej9->_jitConfig)->isRomClassForMethodInSharedCache(j9method, fej9->_jitConfig->javaVM))
            {
            bool sameLoaders = false;
            TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
            if (resolveAOTMethods ||
                (sameLoaders = fej9->sameClassLoaders(clazzOfInlinedMethod, clazzOfCompiledMethod)) ||
                isSystemClassLoader)
               {
               resolvedMethod = new (comp->trHeapMemory()) TR_ResolvedRelocatableJ9Method((TR_OpaqueMethodBlock *) j9method, fe, comp->trMemory(), owningMethod, vTableSlot);
               if (!resolvedMethod) throw std::bad_alloc();
               }
            }
         }

#endif
      }

   packMethodInfo(methodInfo, resolvedMethod, fe);
   }

void
TR_ResolvedJ9JITaaSServerMethod::packMethodInfo(TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo, TR_ResolvedJ9Method *resolvedMethod, TR_FrontEnd *fe)
   {
   auto &methodInfoStruct = std::get<0>(methodInfo);
   if (!resolvedMethod)
      {
      // resolved method not created, setting remoteMirror to NULL indicates
      // that other fields should be ignored.
      methodInfoStruct.remoteMirror = NULL;
      return;
      }


   TR::Compilation *comp = TR::comp();

   // retrieve all relevant attributes of resolvedMethod and set them in methodInfo
   J9RAMConstantPoolItem *literals = (J9RAMConstantPoolItem *)(J9_CP_FROM_METHOD(resolvedMethod->ramMethod()));
   J9Class *cpHdr = J9_CLASS_FROM_CP(literals);

   J9Method *j9method = resolvedMethod->ramMethod();
  
   // fill-in struct fields 
   methodInfoStruct.remoteMirror = resolvedMethod;
   methodInfoStruct.literals = literals;
   methodInfoStruct.ramClass = cpHdr;
   methodInfoStruct.methodIndex = getMethodIndexUnchecked(j9method);
   methodInfoStruct.jniProperties = resolvedMethod->getJNIProperties();
   methodInfoStruct.jniTargetAddress = resolvedMethod->getJNITargetAddress();
   methodInfoStruct.isInterpreted = resolvedMethod->isInterpreted();
   methodInfoStruct.isJNINative = resolvedMethod->isJNINative();
   methodInfoStruct.isMethodInValidLibrary = resolvedMethod->isMethodInValidLibrary();
   methodInfoStruct.mandatoryRm = resolvedMethod->getMandatoryRecognizedMethod();
   methodInfoStruct.rm = ((TR_ResolvedMethod*)resolvedMethod)->getRecognizedMethod();
   methodInfoStruct.startAddressForJittedMethod = TR::CompilationInfo::isCompiled(resolvedMethod->ramMethod()) ?
                                           resolvedMethod->startAddressForJittedMethod() : nullptr;
   methodInfoStruct.virtualMethodIsOverridden = resolvedMethod->virtualMethodIsOverridden();
   methodInfoStruct.addressContainingIsOverriddenBit = resolvedMethod->addressContainingIsOverriddenBit();
   methodInfoStruct.classLoader = resolvedMethod->getClassLoader();

   TR_PersistentJittedBodyInfo *bodyInfo = nullptr;
   // Method may not have been compiled
   if (!resolvedMethod->isInterpreted() && !resolvedMethod->isJITInternalNative())
      {
      bodyInfo = resolvedMethod->getExistingJittedBodyInfo();
      }

   // set string info fields (they cannot be inside of the struct)
   std::string jbi = bodyInfo ? std::string((char*)bodyInfo, sizeof(TR_PersistentJittedBodyInfo)) : std::string();
   std::string methodInfoStr = bodyInfo ? std::string((char*)bodyInfo->getMethodInfo(), sizeof(TR_PersistentMethodInfo)) : std::string();
   std::get<1>(methodInfo) = jbi;
   std::get<2>(methodInfo) = methodInfoStr;

   // set IP method data string.
   // fanin info is not used at cold opt level, so there is no point sending this information to the server
   TR_JITaaSClientIProfiler *iProfiler = (TR_JITaaSClientIProfiler *)((TR_J9VMBase *) fe)->getIProfiler();
   std::get<3>(methodInfo) = (comp && comp->getOptLevel() >= warm && iProfiler) ? iProfiler->serializeIProfilerMethodEntry(resolvedMethod->getPersistentIdentifier()) : std::string();
   }

void
TR_ResolvedJ9JITaaSServerMethod::unpackMethodInfo(TR_OpaqueMethodBlock * aMethod, TR_FrontEnd * fe, TR_Memory * trMemory, uint32_t vTableSlot, TR::CompilationInfoPerThread *threadCompInfo, const TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo)
   {
   auto methodInfoStruct = std::get<0>(methodInfo);
   
   
   _ramMethod = (J9Method *)aMethod;

   _remoteMirror = methodInfoStruct.remoteMirror;

   // Cache the constantPool and constantPoolHeader
   _literals = methodInfoStruct.literals;
   _ramClass = methodInfoStruct.ramClass;

   _romClass = threadCompInfo->getAndCacheRemoteROMClass(_ramClass, trMemory);
   _romMethod = romMethodAtClassIndex(_romClass, methodInfoStruct.methodIndex);
   _romLiterals = (J9ROMConstantPoolItem *) ((UDATA) _romClass + sizeof(J9ROMClass));

   _vTableSlot = vTableSlot;
   _j9classForNewInstance = nullptr;

   _jniProperties = methodInfoStruct.jniProperties;
   _jniTargetAddress = methodInfoStruct.jniTargetAddress;

   _isInterpreted = methodInfoStruct.isInterpreted;
   _isJNINative = methodInfoStruct.isJNINative;
   _isMethodInValidLibrary = methodInfoStruct.isMethodInValidLibrary;
   
   TR::RecognizedMethod mandatoryRm = methodInfoStruct.mandatoryRm;
   TR::RecognizedMethod rm = methodInfoStruct.rm;

   _startAddressForJittedMethod = methodInfoStruct.startAddressForJittedMethod;
   _virtualMethodIsOverridden = methodInfoStruct.virtualMethodIsOverridden;
   _addressContainingIsOverriddenBit = methodInfoStruct.addressContainingIsOverriddenBit;
   _classLoader = methodInfoStruct.classLoader;

   auto &bodyInfoStr = std::get<1>(methodInfo);
   auto &methodInfoStr = std::get<2>(methodInfo);

   _bodyInfo = J9::Recompilation::persistentJittedBodyInfoFromString(bodyInfoStr, methodInfoStr, trMemory);
 
   // initialization from TR_J9Method constructor
   _className = J9ROMCLASS_CLASSNAME(_romClass);
   _name = J9ROMMETHOD_GET_NAME(_romClass, _romMethod);
   _signature = J9ROMMETHOD_GET_SIGNATURE(_romClass, _romMethod);
   parseSignature(trMemory);
   _fullSignature = nullptr;
  
   setMandatoryRecognizedMethod(mandatoryRm);
   setRecognizedMethod(rm);

   TR_JITaaSIProfiler *iProfiler = (TR_JITaaSIProfiler *) ((TR_J9VMBase *) fe)->getIProfiler();
   const std::string entryStr = std::get<3>(methodInfo);
   const auto serialEntry = (TR_ContiguousIPMethodHashTableEntry*) &entryStr[0];
   _iProfilerMethodEntry = (iProfiler && !entryStr.empty()) ? iProfiler->deserializeMethodEntry(serialEntry, trMemory) : nullptr; 
   }

void
TR_ResolvedJ9JITaaSServerMethod::cacheResolvedMethod(TR_ResolvedMethodKey key, TR_OpaqueMethodBlock *method, uint32_t vTableSlot, TR_ResolvedMethod *resolvedMethod, TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo)
   {
   static bool useCaching = !feGetEnv("TR_DisableResolvedMethodsCaching");
   if (!useCaching)
      return;

   TR_ASSERT(std::get<0>(methodInfo).remoteMirror, "Remote mirror must not be NULL");

   if (!resolvedMethod)
      // Do not cache unresolved methods
      return;

   static bool useLocalCaching = !feGetEnv("TR_DisableResolvedMethodsLocalCaching");
   if (useLocalCaching)
      {
      // First, cache locally
      _resolvedMethodsCache.insert({key, (TR_ResolvedJ9Method *) resolvedMethod});
      }

   static bool useGlobalCaching = !feGetEnv("TR_DisableResolvedMethodsGlobalCaching");
   if (useGlobalCaching)
      {
      // Now, cache globally
      auto compInfoPT = _fe->_compInfoPT;
      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor());
      // Look into a cache of an owning J9Method
      auto &resolvedMethodInfoMap = getJ9ClassInfo(compInfoPT, _ramClass)._resolvedMethodInfoCache;
      if (!resolvedMethodInfoMap)
         {
         // Cache not initialized yet
         resolvedMethodInfoMap = new (PERSISTENT_NEW) TR_ResolvedMethodInfoCache(TR_ResolvedMethodInfoCache::allocator_type(TR::Compiler->persistentAllocator()));
         }

      auto compID = ((TR_J9ServerVM *) _fe)->getCompilationID();
      resolvedMethodInfoMap->insert({key, {compID, method, vTableSlot, methodInfo}});
      }
   }

// retrieve a resolved method from the cache
// returns true if the method is cached, sets resolvedMethod and unresolvedInCP to the cached values.
// returns false otherwise.
bool 
TR_ResolvedJ9JITaaSServerMethod::getCachedResolvedMethod(TR_ResolvedMethodKey key, TR_ResolvedMethod **resolvedMethod, bool *unresolvedInCP)
   {
   // First, check a local cache
   auto localIt = _resolvedMethodsCache.find(key);
   if (localIt != _resolvedMethodsCache.end())
      {
      if (unresolvedInCP)
         *unresolvedInCP = false;
      if (resolvedMethod)
         {
         *resolvedMethod = localIt->second;
         auto comp = _fe->_compInfoPT->getCompilation();
         if (comp->compileRelocatableCode() && comp->getOption(TR_UseSymbolValidationManager))
            {
            auto serverMethod = static_cast<TR_ResolvedJ9JITaaSServerMethod *>(*resolvedMethod);
            if(!addValidationRecordForCachedResolvedMethod(
                  key,
                  (TR_OpaqueMethodBlock *) serverMethod->ramMethod(),
                  comp))
               {
               // Could not add a validation record
               *resolvedMethod = NULL;
               if (*unresolvedInCP) *unresolvedInCP = true;
               }
            }
         }
      return true;
      }

   // Now, check the global cache
   auto compInfoPT = _fe->_compInfoPT;
   OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor());
   // Look into a cache of an owning J9Method
   auto &resolvedMethodInfoMap = getJ9ClassInfo(compInfoPT, _ramClass)._resolvedMethodInfoCache;
   if (!resolvedMethodInfoMap)
      {
      // Cache not initialized yet
      return false;
      }
   auto cacheIt = resolvedMethodInfoMap->find(key);
   if (cacheIt != resolvedMethodInfoMap->end())
      {
      // Found entry in the global cache
      auto comp = compInfoPT->getCompilation();
      auto methodCacheEntry = cacheIt->second;
      auto methodInfo = methodCacheEntry.methodInfo;
      TR_OpaqueMethodBlock *method = methodCacheEntry.method;
      uint32_t vTableSlot = methodCacheEntry.vTableSlot;
      
      if (comp->compileRelocatableCode() && comp->getOption(TR_UseSymbolValidationManager))
         {
         if(!addValidationRecordForCachedResolvedMethod(
               key,
               methodCacheEntry.method,
               comp))
            {
            // Could not add a validation record
            if (*resolvedMethod) *resolvedMethod = NULL;
            if (*unresolvedInCP) *unresolvedInCP = true;
            return true;
            }
         }

      // Create resolved method from cached method info
      if (key.type != TR_ResolvedMethodType::VirtualFromOffset)
         {
         *resolvedMethod = createResolvedMethodFromJ9Method(
                              comp,
                              key.cpIndex,
                              vTableSlot,
                              (J9Method *) method,
                              unresolvedInCP,
                              NULL,
                              methodInfo);
         }
      else
         {
         if (_fe->isAOT_DEPRECATED_DO_NOT_USE())
            *resolvedMethod = method ? new (comp->trHeapMemory()) TR_ResolvedRelocatableJ9JITaaSServerMethod(method, _fe, comp->trMemory(), methodInfo, this) : 0;
         else
            *resolvedMethod = method ? new (comp->trHeapMemory()) TR_ResolvedJ9JITaaSServerMethod(method, _fe, comp->trMemory(), methodInfo, this) : 0;
         }
      if (*resolvedMethod)
         {
         if (unresolvedInCP) *unresolvedInCP = false;
         auto currentCompID = ((TR_J9ServerVM *) _fe)->getCompilationID();
         auto cachedCompID = methodCacheEntry.compID;
         if (currentCompID != cachedCompID)
            {
            // Resolved method was cached during a different compilation,
            // so its client-side mirror might have been destroyed by now.
            // remoteMirror = NULL signals that it needs to be recreated
            ((TR_ResolvedJ9JITaaSServerMethod *) *resolvedMethod)->setRemoteMirror(NULL);
            }

         return true;
         }
      else
         {
         TR_ASSERT(false, "Should not have cached unresolved method globally");
         }
      }
   return false;
   }

bool
TR_ResolvedJ9JITaaSServerMethod::validateMethodFieldAttributes(const TR_J9MethodFieldAttributes &attributes, bool isStatic, int32_t cpIndex, bool isStore, bool needAOTValidation)
   {
   if (!isStatic)
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_fieldAttributes, _remoteMirror, cpIndex, isStore, needAOTValidation, getResolvedMethodParams());
   else
      _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_staticAttributes, _remoteMirror, cpIndex, isStore, needAOTValidation, getResolvedMethodParams());
   auto recv = _stream->read<TR_J9MethodFieldAttributes, TR_ResolvedJ9Method *>();
   auto clientAttributes = std::get<0>(recv);
   if (!_remoteMirror)
      _remoteMirror = std::get<1>(recv);
   bool equal = attributes == clientAttributes;
   return equal;
   }

TR_ResolvedMethodKey
TR_ResolvedJ9JITaaSServerMethod::getResolvedMethodKey(TR_ResolvedMethodType type, int32_t cpIndex, TR_OpaqueClassBlock *classObject)
   {
   TR_ResolvedMethodKey key = {type, cpIndex, classObject};
   return key;
   }

TR_ResolvedMethodParams
TR_ResolvedJ9JITaaSServerMethod::getResolvedMethodParams()
   {
   TR_ResolvedJ9Method *owningMethodMirror = _owningMethod ? ((TR_ResolvedJ9JITaaSServerMethod*) _owningMethod)->_remoteMirror : NULL;
   TR_ResolvedMethodParams owningMethodParams = {_ramMethod, owningMethodMirror, _vTableSlot};
   return owningMethodParams;
   }

TR_ResolvedJ9Method *
TR_ResolvedJ9JITaaSServerMethod::recreateResolvedMethodMirror(const TR_ResolvedMethodParams &methodParams, TR_J9VMBase *fe, TR_Memory *trMemory)
   {
   // Should be called on the client to recreate a resolved method mirror from the
   // parameters sent by the server
   TR_ResolvedJ9JITaaSServerMethodInfo methodInfo;
   if (methodParams.owningMethod)
      TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodFromJ9MethodMirror(methodInfo, (TR_OpaqueMethodBlock *) methodParams.ramMethod, methodParams.vTableSlot, methodParams.owningMethod, fe, trMemory);
   else
      TR_ResolvedJ9JITaaSServerMethod::createResolvedMethodMirror(methodInfo, (TR_OpaqueMethodBlock *) methodParams.ramMethod, 0, methodParams.owningMethod, fe, trMemory);
   return std::get<0>(methodInfo).remoteMirror;
   }

void
TR_ResolvedJ9JITaaSServerMethod::recreateRemoteMirrorIfNecessary()
   {
   // if this method's _remoteMirror = NULL, it means that we fetched it
   // from global cache, thus its remote mirror might not exist anymore.
   // Make a remote call to create a new mirror on the client and update
   // _remoteMirror pointer
   if (_remoteMirror)
      return;

   TR_ResolvedJ9Method *owningMethodMirror = _owningMethod ? ((TR_ResolvedJ9JITaaSServerMethod*) _owningMethod)->_remoteMirror : nullptr;
   _stream->write(JITaaS::J9ServerMessageType::mirrorResolvedJ9Method, (TR_OpaqueMethodBlock *) _ramMethod, owningMethodMirror, _vTableSlot, fej9()->isAOT_DEPRECATED_DO_NOT_USE());
   auto recv = _stream->read<TR_ResolvedJ9JITaaSServerMethodInfo>();
   auto &methodInfo = std::get<0>(recv);
   // _remoteMirror = std::get<0>(methodInfo).remoteMirror;
   unpackMethodInfo((TR_OpaqueMethodBlock *) _ramMethod, _fe, _fe->_compInfoPT->getCompilation()->trMemory(), _vTableSlot, _fe->_compInfoPT, methodInfo);
   }

bool
TR_ResolvedJ9JITaaSServerMethod::addValidationRecordForCachedResolvedMethod(const TR_ResolvedMethodKey &key, TR_OpaqueMethodBlock *ramMethod, TR::Compilation *comp)
   {
   // This method should be called whenever a resolved method is fetched from
   // cache during AOT compilation.
   // There are 2 reasons for this:
   // 1. If the cached resolved method was created while in heuristic region,
   // SVM record would not be added.
   // 2. If resolved method is fetched from the global cache, it might be retrieved
   // during a later compilation. Since SVM records are per-compilation, will need to
   // add them again.
   auto svm = comp->getSymbolValidationManager();
   int32_t cpIndex = key.cpIndex;
   TR_OpaqueClassBlock *classObject = key.classObject;
   bool added = false;
   switch (key.type)
      {
      case VirtualFromCP:
         added = svm->addVirtualMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex);
         break;
      case VirtualFromOffset:
         added = svm->addVirtualMethodFromOffsetRecord(ramMethod, classObject, key.cpIndex, false);
         break;
      case Interface:
         added = svm->addInterfaceMethodFromCPRecord(
            (TR_OpaqueMethodBlock *) ramMethod,
            (TR_OpaqueClassBlock *) ((TR_J9VM *) _fe)->getClassFromMethodBlock(getPersistentIdentifier()),
            classObject,
            cpIndex);
         break;
      case Static:
         added = svm->addStaticMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex);
         break;
      case Special:
         added = svm->addSpecialMethodFromCPRecord((TR_OpaqueMethodBlock *)ramMethod, cp(), cpIndex);
         break;
      default:
         TR_ASSERT(false, "Invalid TR_ResolvedMethodType value");
      }
   return added;
   }

TR_ResolvedRelocatableJ9JITaaSServerMethod::TR_ResolvedRelocatableJ9JITaaSServerMethod(TR_OpaqueMethodBlock * aMethod, TR_FrontEnd * fe, TR_Memory * trMemory, TR_ResolvedMethod * owner, uint32_t vTableSlot)
   : TR_ResolvedJ9JITaaSServerMethod(aMethod, fe, trMemory, owner, vTableSlot)
   {
   // NOTE: avoid using this constructor as much as possible.
   // Using may result in multiple remote messages (up to 3?) sent to the client
   TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
   TR::Compilation *comp = fej9->_compInfoPT->getCompilation();
   if (comp && this->TR_ResolvedMethod::getRecognizedMethod() != TR::unknownMethod)
      {
      if (fej9->canRememberClass(containingClass()))
         {
         if (comp->getOption(TR_UseSymbolValidationManager))
            {
            TR::SymbolValidationManager *svm = comp->getSymbolValidationManager();
            SVM_ASSERT_ALREADY_VALIDATED(svm, aMethod);
            SVM_ASSERT_ALREADY_VALIDATED(svm, containingClass());
            }
         else
            {
            ((TR_ResolvedRelocatableJ9JITaaSServerMethod *) owner)->validateArbitraryClass(comp, (J9Class*)containingClass());
            }
         }
      }
   }

TR_ResolvedRelocatableJ9JITaaSServerMethod::TR_ResolvedRelocatableJ9JITaaSServerMethod(TR_OpaqueMethodBlock * aMethod, TR_FrontEnd * fe, TR_Memory * trMemory, const TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo, TR_ResolvedMethod * owner, uint32_t vTableSlot)
   : TR_ResolvedJ9JITaaSServerMethod(aMethod, fe, trMemory, methodInfo, owner, vTableSlot)
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
   TR::Compilation *comp = fej9->_compInfoPT->getCompilation();
   if (comp && this->TR_ResolvedMethod::getRecognizedMethod() != TR::unknownMethod)
      {
      // in the client-side constructor we check if containing class has been remembered,
      // and if it wasn't set recognized method to TR::unknownMethod.
      // However, this constructor is passed methodInfo, which means that client-side mirror
      // has already been created, attempted to remember the containing class and set recognized method
      // to appropriate value, then wrote it to methodInfo. So, we don't need that check on the server.
      if (comp->getOption(TR_UseSymbolValidationManager))
         {
         TR::SymbolValidationManager *svm = comp->getSymbolValidationManager();
         SVM_ASSERT_ALREADY_VALIDATED(svm, aMethod);
         SVM_ASSERT_ALREADY_VALIDATED(svm, containingClass());
         }
      else
         {
         // TODO: this will require more remote messages.
         // For simplicity, leave it like this for now, once testing can be done, optimize it
         ((TR_ResolvedRelocatableJ9JITaaSServerMethod *) owner)->validateArbitraryClass(comp, (J9Class*)containingClass());
         }
      }
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::constantPool()
   {
   return romLiterals();
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::isInterpreted()
   {
   bool alwaysTreatAsInterpreted = true;
#if defined(TR_TARGET_S390)
   alwaysTreatAsInterpreted = false;
#elif defined(TR_TARGET_X86)

   /*if isInterpreted should be only overridden for JNI methods.
   Otherwise buildDirectCall in X86PrivateLinkage.cpp will generate CALL 0
   for certain jitted methods as startAddressForJittedMethod still returns NULL in AOT
   this is not an issue on z as direct calls are dispatched via snippets

   If one of the options to disable JNI is specified
   this function reverts back to the old behaviour
   */
   TR::Compilation *comp = _fe->_compInfoPT->getCompilation();
   if (isJNINative() &&
        !comp->getOption(TR_DisableDirectToJNI)  &&
        !comp->getOption(TR_DisableDirectToJNIInline) &&
        !comp->getOption(TR_DisableDirectToJNI) &&
        !comp->getOption(TR_DisableDirectToJNIInline)
        )
      {
      alwaysTreatAsInterpreted = false;
      }
#endif

   if (alwaysTreatAsInterpreted)
      return true;

   return TR_ResolvedJ9JITaaSServerMethod::isInterpreted();
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::isInterpretedForHeuristics()
   {
   return TR_ResolvedJ9JITaaSServerMethod::isInterpreted();
   }

TR_OpaqueMethodBlock *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getNonPersistentIdentifier()
   {
   return (TR_OpaqueMethodBlock *)ramMethod();
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::validateArbitraryClass(TR::Compilation *comp, J9Class *clazz)
   {
   return storeValidationRecordIfNecessary(comp, cp(), 0, TR_ValidateArbitraryClass, ramMethod(), clazz);
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::storeValidationRecordIfNecessary(TR::Compilation * comp, J9ConstantPool *constantPool, int32_t cpIndex, TR_ExternalRelocationTargetKind reloKind, J9Method *ramMethod, J9Class *definingClass)
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *) comp->fe();
   bool storeClassInfo = true;
   bool fieldInfoCanBeUsed = false;
   TR_AOTStats *aotStats = ((TR_JitPrivateConfig *)fej9->_jitConfig->privateConfig)->aotStats;
   bool isStatic = (reloKind == TR_ValidateStaticField);

   _stream->write(JITaaS::J9ServerMessageType::ResolvedRelocatableMethod_storeValidationRecordIfNecessary, ramMethod, constantPool, cpIndex, isStatic, definingClass);
   // 1. RAM class of ramMethod
   // 2. defining class
   // 3. class chain
   auto recv = _stream->read<J9Class *, J9Class *, UDATA *>();

   J9Class *clazz = std::get<0>(recv);
   traceMsg(comp, "storeValidationRecordIfNecessary:\n");
   traceMsg(comp, "\tconstantPool %p cpIndex %d\n", constantPool, cpIndex);
   traceMsg(comp, "\treloKind %d isStatic %d\n", reloKind, isStatic);
   J9UTF8 *methodClassName = J9ROMCLASS_CLASSNAME(TR::Compiler->cls.romClassOf((TR_OpaqueClassBlock *) clazz));
   traceMsg(comp, "\tmethod %p from class %p %.*s\n", ramMethod, clazz, J9UTF8_LENGTH(methodClassName), J9UTF8_DATA(methodClassName));
   traceMsg(comp, "\tdefiningClass %p\n", definingClass);

   if (!definingClass)
      {
      definingClass = std::get<1>(recv);
      traceMsg(comp, "\tdefiningClass recomputed from cp as %p\n", definingClass);
      }

   if (!definingClass)
      {
      if (aotStats)
         aotStats->numDefiningClassNotFound++;
      return false;
      }

   J9UTF8 *className = J9ROMCLASS_CLASSNAME(TR::Compiler->cls.romClassOf((TR_OpaqueClassBlock *) definingClass));
   traceMsg(comp, "\tdefiningClass name %.*s\n", J9UTF8_LENGTH(className), J9UTF8_DATA(className));

   J9ROMClass *romClass = NULL;
   // all kinds of validations may need to rely on the entire class chain, so make sure we can build one first
   void *classChain = (void *) std::get<2>(recv);

   if (!classChain)
      return false;

   bool inLocalList = false;
   TR::list<TR::AOTClassInfo*>* aotClassInfo = comp->_aotClassInfo;
   if (!aotClassInfo->empty())
      {
      for (auto info = aotClassInfo->begin(); info != aotClassInfo->end(); ++info)
         {
         TR_ASSERT(((*info)->_reloKind == TR_ValidateInstanceField ||
                 (*info)->_reloKind == TR_ValidateStaticField ||
                 (*info)->_reloKind == TR_ValidateClass ||
                 (*info)->_reloKind == TR_ValidateArbitraryClass),
                "TR::AOTClassInfo reloKind is not TR_ValidateInstanceField or TR_ValidateStaticField or TR_ValidateClass!");

         if ((*info)->_reloKind == reloKind)
            {
            if (isStatic)
               inLocalList = (romClass == TR::Compiler->cls.romClassOf((TR_OpaqueClassBlock *) ((*info)->_clazz)));
            else
               inLocalList = (classChain == (*info)->_classChain &&
                              cpIndex == (*info)->_cpIndex &&
                              ramMethod == (J9Method *)(*info)->_method);

            if (inLocalList)
               break;
            }
         }
      }

   if (inLocalList)
      {
      traceMsg(comp, "\tFound in local list, nothing to do\n");
      if (aotStats)
         {
         if (isStatic)
            aotStats->numStaticEntriesAlreadyStoredInLocalList++;
         else
            aotStats->numCHEntriesAlreadyStoredInLocalList++;
         }
      return true;
      }

   TR::AOTClassInfo *classInfo = new (comp->trHeapMemory()) TR::AOTClassInfo(fej9, (TR_OpaqueClassBlock *)definingClass, (void *) classChain, (TR_OpaqueMethodBlock *)ramMethod, cpIndex, reloKind);
   if (classInfo)
      {
      traceMsg(comp, "\tCreated new AOT class info %p\n", classInfo);
      comp->_aotClassInfo->push_front(classInfo);
      if (aotStats)
         {
         if (isStatic)
            aotStats->numNewStaticEntriesInLocalList++;
         else
            aotStats->numNewCHEntriesInLocalList++;
         }

      return true;
      }

   // should only be a native OOM that gets us here...
   return false;
   }

U_8 *
TR_ResolvedRelocatableJ9JITaaSServerMethod::allocateException(uint32_t numBytes, TR::Compilation *comp)
   {
   J9JITExceptionTable *eTbl = NULL;
   uint32_t size = 0;
   bool shouldRetryAllocation;
   eTbl = (J9JITExceptionTable *)_fe->allocateDataCacheRecord(numBytes, comp, _fe->needsContiguousAllocation(), &shouldRetryAllocation,
                                                              J9_JIT_DCE_EXCEPTION_INFO, &size);
   if (!eTbl)
      {
      if (shouldRetryAllocation)
         {
         // force a retry
         comp->failCompilation<J9::RecoverableDataCacheError>("Failed to allocate exception table");
         }
      comp->failCompilation<J9::DataCacheError>("Failed to allocate exception table");
      }
   memset((uint8_t *)eTbl, 0, size);

   // These get updated in TR_RelocationRuntime::relocateAOTCodeAndData
   eTbl->ramMethod = NULL;
   eTbl->constantPool = NULL;

   return (U_8 *) eTbl;
   }

TR_ResolvedMethod *
TR_ResolvedRelocatableJ9JITaaSServerMethod::createResolvedMethodFromJ9Method(TR::Compilation *comp, I_32 cpIndex, uint32_t vTableSlot, J9Method *j9method, bool * unresolvedInCP, TR_AOTInliningStats *aotStats) {
   // This method is called when a remote mirror hasn't been created yet, so it needs to be created from here.
   TR_ResolvedMethod *resolvedMethod = NULL;

#if defined(J9VM_OPT_SHARED_CLASSES) && (defined(TR_HOST_X86) || defined(TR_HOST_POWER) || defined(TR_HOST_S390) || defined(TR_HOST_ARM))
   static char *dontInline = feGetEnv("TR_AOTDontInline");

   if (dontInline)
      return NULL;

   // This message will create a remote mirror.
   // Calling constructor would be simpler, but we would have to make another message to update stats
   _stream->write(JITaaS::J9ServerMessageType::ResolvedRelocatableMethod_createResolvedRelocatableJ9Method, getRemoteMirror(), j9method, cpIndex, vTableSlot);
   auto recv = _stream->read<TR_ResolvedJ9JITaaSServerMethodInfo, bool, bool, bool>();
   auto methodInfo = std::get<0>(recv);
   // These parameters are only needed to update AOT stats. 
   // Maybe we shouldn't even track them, because another version of this method doesn't.
   bool isRomClassForMethodInSharedCache = std::get<1>(recv);
   bool sameClassLoaders = std::get<2>(recv);
   bool sameClass = std::get<3>(recv);

   if (std::get<0>(methodInfo).remoteMirror)
      {
      // Remote mirror created successfully
      resolvedMethod = new (comp->trHeapMemory()) TR_ResolvedRelocatableJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) j9method, _fe, comp->trMemory(), methodInfo, this, vTableSlot);
      if (aotStats)
         {
         aotStats->numMethodResolvedAtCompile++;
         if (sameClass)
            aotStats->numMethodInSameClass++;
         else
            aotStats->numMethodNotInSameClass++;
         }
      }
   else if (aotStats)
      {
      // Remote mirror not created, update AOT stats
      if (!isRomClassForMethodInSharedCache)
         aotStats->numMethodROMMethodNotInSC++;
      else if (!sameClassLoaders)
         aotStats->numMethodFromDiffClassLoader++;
      }

#endif

   return resolvedMethod;
   }

TR_ResolvedMethod *
TR_ResolvedRelocatableJ9JITaaSServerMethod::createResolvedMethodFromJ9Method( TR::Compilation *comp, int32_t cpIndex, uint32_t vTableSlot, J9Method *j9method, bool * unresolvedInCP, TR_AOTInliningStats *aotStats, const TR_ResolvedJ9JITaaSServerMethodInfo &methodInfo)
   {
   // If this method is called, remote mirror has either already been created or creation failed.
   // In either case, methodInfo contains all parameters required to construct a resolved method or check that it's NULL.
   // The only problem is that we can't update AOT stats from this method.
   TR_ResolvedMethod *resolvedMethod = NULL;
   if (std::get<0>(methodInfo).remoteMirror)
      resolvedMethod = new (comp->trHeapMemory()) TR_ResolvedRelocatableJ9JITaaSServerMethod((TR_OpaqueMethodBlock *) j9method, _fe, comp->trMemory(), methodInfo, this, vTableSlot);

   return resolvedMethod;
   }

TR_ResolvedMethod *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getResolvedImproperInterfaceMethod(
   TR::Compilation * comp,
   I_32 cpIndex)
   {
   if (comp->getOption(TR_UseSymbolValidationManager))
      return TR_ResolvedJ9JITaaSServerMethod::getResolvedImproperInterfaceMethod(comp, cpIndex);

   // For now leave private and Object invokeinterface unresolved in AOT. If we
   // resolve it, we may forceUnresolvedDispatch in codegen, in which case the
   // generated code would attempt to resolve the wrong kind of constant pool
   // entry.
   return NULL;
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::startAddressForJittedMethod()
   {
   return NULL;
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::startAddressForJNIMethod(TR::Compilation * comp)
   {
#if defined(TR_TARGET_S390)  || defined(TR_TARGET_X86) || defined(TR_TARGET_POWER)
   return TR_ResolvedJ9JITaaSServerMethod::startAddressForJNIMethod(comp);
#else
   return NULL;
#endif
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::startAddressForJITInternalNativeMethod()
   {
   return NULL;
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::startAddressForInterpreterOfJittedMethod()
   {
   return ((J9Method *)getNonPersistentIdentifier())->extra;
   }

TR_OpaqueClassBlock *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getClassFromConstantPool(TR::Compilation *comp, uint32_t cpIndex, bool returnClassForAOT)
   {
   if (returnClassForAOT || comp->getOption(TR_UseSymbolValidationManager))
      {
      TR_OpaqueClassBlock * resolvedClass = TR_ResolvedJ9JITaaSServerMethod::getClassFromConstantPool(comp, cpIndex, returnClassForAOT);
      if (resolvedClass &&
         validateClassFromConstantPool(comp, (J9Class *)resolvedClass, cpIndex))
         {
         return (TR_OpaqueClassBlock*)resolvedClass;
         }
      }
   return NULL;
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::validateClassFromConstantPool(TR::Compilation *comp, J9Class *clazz, uint32_t cpIndex,  TR_ExternalRelocationTargetKind reloKind)
   {
   if (comp->getOption(TR_UseSymbolValidationManager))
      {
      return comp->getSymbolValidationManager()->addClassFromCPRecord(reinterpret_cast<TR_OpaqueClassBlock *>(clazz), cp(), cpIndex);
      }
   else
      {
      return storeValidationRecordIfNecessary(comp, cp(), cpIndex, reloKind, ramMethod(), clazz);
      }
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::isUnresolvedString(I_32 cpIndex, bool optimizeForAOT)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");

   if (optimizeForAOT)
      return TR_ResolvedJ9JITaaSServerMethod::isUnresolvedString(cpIndex);
   return true;
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::methodTypeConstant(I_32 cpIndex)
   {
   TR_ASSERT(false, "should be unreachable");
   return NULL;
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::isUnresolvedMethodType(I_32 cpIndex)
   {
   TR_ASSERT(false, "should be unreachable");
   return true;
   }

void *
TR_ResolvedRelocatableJ9JITaaSServerMethod::methodHandleConstant(I_32 cpIndex)
   {
   TR_ASSERT(false, "should be unreachable");
   return NULL;
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::isUnresolvedMethodHandle(I_32 cpIndex)
   {
   TR_ASSERT(false, "should be unreachable");
   return true;
   }

char *
TR_ResolvedRelocatableJ9JITaaSServerMethod::fieldOrStaticNameChars(I_32 cpIndex, int32_t & len)
   {
   len = 0;
   return "";
   }

TR_OpaqueClassBlock *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getDeclaringClassFromFieldOrStatic(TR::Compilation *comp, int32_t cpIndex)
   {
   TR_OpaqueClassBlock *definingClass = TR_ResolvedJ9JITaaSServerMethod::getDeclaringClassFromFieldOrStatic(comp, cpIndex);
   if (comp->getOption(TR_UseSymbolValidationManager))
      {
      if (!comp->getSymbolValidationManager()->addDeclaringClassFromFieldOrStaticRecord(definingClass, cp(), cpIndex))
         return NULL;
      }
   return definingClass;
   }

TR_OpaqueClassBlock *
TR_ResolvedRelocatableJ9JITaaSServerMethod::classOfStatic(int32_t cpIndex, bool returnClassForAOT)
   {
   TR_OpaqueClassBlock * clazz = TR_ResolvedJ9JITaaSServerMethod::classOfStatic(cpIndex, returnClassForAOT);

   TR::Compilation *comp = TR::comp();
   bool validated = false;

   if (comp && comp->getOption(TR_UseSymbolValidationManager))
      {
      validated = comp->getSymbolValidationManager()->addStaticClassFromCPRecord(clazz, cp(), cpIndex);
      }
   else
      {
      validated = returnClassForAOT;
      }

   if (validated)
      return clazz;
   else
      return NULL;
   }

TR_ResolvedMethod *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getResolvedPossiblyPrivateVirtualMethod(
   TR::Compilation *comp,
   int32_t cpIndex,
   bool ignoreRtResolve,
   bool * unresolvedInCP)
   {
   TR_ResolvedMethod *method =
      TR_ResolvedJ9JITaaSServerMethod::getResolvedPossiblyPrivateVirtualMethod(
         comp,
         cpIndex,
         ignoreRtResolve,
         unresolvedInCP);

   if (comp->getOption(TR_UseSymbolValidationManager))
      return method;

   // For now leave private invokevirtual unresolved in AOT. If we resolve it,
   // we may forceUnresolvedDispatch in codegen, in which case the generated
   // code would attempt to resolve the wrong kind of constant pool entry.
   return (method == NULL || method->isPrivate()) ? NULL : method;
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::getUnresolvedFieldInCP(I_32 cpIndex)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
#if defined(J9VM_INTERP_AOT_COMPILE_SUPPORT) && defined(J9VM_OPT_SHARED_CLASSES) && (defined(TR_HOST_X86) || defined(TR_HOST_POWER) || defined(TR_HOST_S390) || defined(TR_HOST_ARM))
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getUnresolvedFieldInCP, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<bool>());
#else
   return true;
#endif
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::getUnresolvedStaticMethodInCP(int32_t cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getUnresolvedStaticMethodInCP, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<bool>());
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::getUnresolvedSpecialMethodInCP(I_32 cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedMethod_getUnresolvedSpecialMethodInCP, getRemoteMirror(), cpIndex);
   return std::get<0>(_stream->read<bool>());
   }

void
TR_ResolvedRelocatableJ9JITaaSServerMethod::handleUnresolvedStaticMethodInCP(int32_t cpIndex, bool * unresolvedInCP)
   {
   *unresolvedInCP = getUnresolvedStaticMethodInCP(cpIndex);
   }

void
TR_ResolvedRelocatableJ9JITaaSServerMethod::handleUnresolvedSpecialMethodInCP(int32_t cpIndex, bool * unresolvedInCP)
   {
   *unresolvedInCP = getUnresolvedSpecialMethodInCP(cpIndex);
   }

void
TR_ResolvedRelocatableJ9JITaaSServerMethod::handleUnresolvedVirtualMethodInCP(int32_t cpIndex, bool * unresolvedInCP)
   {
   *unresolvedInCP = getUnresolvedVirtualMethodInCP(cpIndex);
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::getUnresolvedVirtualMethodInCP(int32_t cpIndex)
   {
   return false;
   }

UDATA
TR_ResolvedRelocatableJ9JITaaSServerMethod::getFieldType(J9ROMConstantPoolItem * CP, int32_t cpIndex)
   {
   _stream->write(JITaaS::J9ServerMessageType::ResolvedRelocatableMethod_getFieldType, cpIndex, getRemoteMirror());
   auto recv = _stream->read<UDATA>();
   return (std::get<0>(recv));
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::fieldAttributes(TR::Compilation * comp, int32_t cpIndex, uint32_t * fieldOffset, TR::DataType * type, bool * volatileP, bool * isFinal, bool * isPrivate, bool isStore, bool * unresolvedInCP, bool needAOTValidation)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");
   
   // Will search per-resolved method cache and make a remote call if needed, client will call AOT version of the method.
   // We still need to create a validation record, and if it can't be added, change
   // return values appropriately.
   bool isStatic = false;
   J9ConstantPool *constantPool = (J9ConstantPool *)literals();
   bool theFieldIsFromLocalClass = false;
   TR_OpaqueClassBlock *definingClass = NULL;
   TR_J9MethodFieldAttributes attributes;
   if (!getCachedFieldAttributes(cpIndex, attributes, isStatic))
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedRelocatableMethod_fieldAttributes, getRemoteMirror(), cpIndex, isStore, needAOTValidation);
      auto recv = _stream->read<TR_J9MethodFieldAttributes, TR_OpaqueClassBlock *>();
      attributes = std::get<0>(recv);
      definingClass = std::get<1>(recv);
      
      cacheFieldAttributes(cpIndex, attributes, isStatic);
      }
   else
      {
#if defined(DEBUG) || defined(PROD_WITH_ASSUMES)
      if (!attributes.isUnresolvedInCP())
         {
         // Validate cached attributes from global cache.
         // Do not validate locally cached attributes, because even if they are incorrect
         // they only exist for the duration of one compilation
         bool validated = validateMethodFieldAttributes(attributes, isStatic, cpIndex, isStore, constantPool);
         TR_ASSERT(validated, "static attributes from client and cache do not match");
         }
#endif
      }
   attributes.setMethodFieldAttributesResult(fieldOffset, type, volatileP, isFinal, isPrivate, unresolvedInCP, &theFieldIsFromLocalClass);

   bool fieldInfoCanBeUsed = false;
   bool resolveField = true;

   if (comp->getOption(TR_DisableAOTInstanceFieldResolution))
      {
      resolveField = false;
      }
   else
      {
      if (needAOTValidation)
         {
         if (comp->getOption(TR_UseSymbolValidationManager))
            {
            fieldInfoCanBeUsed = comp->getSymbolValidationManager()->addDefiningClassFromCPRecord(reinterpret_cast<TR_OpaqueClassBlock *> (definingClass), constantPool, cpIndex);
            }
         else
            {
            fieldInfoCanBeUsed = storeValidationRecordIfNecessary(comp, constantPool, cpIndex, TR_ValidateInstanceField, ramMethod());
            }
         }
      else
         {
         fieldInfoCanBeUsed = true;
         }
      }

   if (!resolveField)
      {
      *fieldOffset = (U_32)NULL;
      fieldInfoCanBeUsed = false;
      }

   if (!fieldInfoCanBeUsed)
      {
      theFieldIsFromLocalClass = false;
      // Either couldn't create validation record 
      // or AOT instance field resolution is disabled.
      if (volatileP) *volatileP = true;
      if (isFinal) *isFinal = false;
      if (isPrivate) *isPrivate = false;
      if (fieldOffset) *(U_32*)fieldOffset = (U_32) sizeof(J9Object);
      }

   return theFieldIsFromLocalClass;
   }

bool
TR_ResolvedRelocatableJ9JITaaSServerMethod::staticAttributes(TR::Compilation * comp,
                                                 int32_t cpIndex,
                                                 void * * address,
                                                 TR::DataType * type,
                                                 bool * volatileP,
                                                 bool * isFinal,
                                                 bool * isPrivate,
                                                 bool isStore,
                                                 bool * unresolvedInCP,
                                                 bool needAOTValidation)
   {
   TR_ASSERT(cpIndex != -1, "cpIndex shouldn't be -1");

   // Will search per-resolved method cache and make a remote call if needed, client will call AOT version of the method.
   // We still need to create a validation record, and if it can't be added, change
   // return values appropriately.
   bool isStatic = true;
   J9ConstantPool *constantPool = (J9ConstantPool *)literals();
   bool theFieldIsFromLocalClass = false;
   TR_OpaqueClassBlock *definingClass = NULL;
   TR_J9MethodFieldAttributes attributes;
   if(!getCachedFieldAttributes(cpIndex, attributes, isStatic))
      {
      _stream->write(JITaaS::J9ServerMessageType::ResolvedRelocatableMethod_staticAttributes, getRemoteMirror(), cpIndex, isStore, needAOTValidation);
      auto recv = _stream->read<TR_J9MethodFieldAttributes, TR_OpaqueClassBlock *>();
      attributes = std::get<0>(recv);
      definingClass = std::get<1>(recv);
      
      cacheFieldAttributes(cpIndex, attributes, isStatic);
      }
   else
      {
#if defined(DEBUG) || defined(PROD_WITH_ASSUMES)
      bool validated = validateMethodFieldAttributes(attributes, isStatic, cpIndex, isStore, constantPool);
      TR_ASSERT(validated, "static attributes from client and cache do not match");
#endif
      }
   attributes.setMethodFieldAttributesResult(address, type, volatileP, isFinal, isPrivate, unresolvedInCP, &theFieldIsFromLocalClass);

   bool fieldInfoCanBeUsed = false;
   bool resolveField = true;
   if (comp->getOption(TR_DisableAOTInstanceFieldResolution))
      {
      resolveField = false;
      }
   else
      {
      if (needAOTValidation)
         {
         if (comp->getOption(TR_UseSymbolValidationManager))
            {
            fieldInfoCanBeUsed = comp->getSymbolValidationManager()->addDefiningClassFromCPRecord(reinterpret_cast<TR_OpaqueClassBlock *> (definingClass), constantPool, cpIndex, true);
            }
         else
            {
            fieldInfoCanBeUsed = storeValidationRecordIfNecessary(comp, constantPool, cpIndex, TR_ValidateInstanceField, ramMethod());
            }
         }
      else
         {
         fieldInfoCanBeUsed = true;
         }
      }

   if (!resolveField)
      {
      *address = (U_32)NULL;
      fieldInfoCanBeUsed = false;
      }

   if (!fieldInfoCanBeUsed)
      {
      theFieldIsFromLocalClass = false;
      // Either couldn't create validation record 
      // or AOT instance field resolution is disabled.
      if (volatileP) *volatileP = true;
      if (isFinal) *isFinal = false;
      if (isPrivate) *isPrivate = false;
      if (address) *address = NULL;
      }

   return theFieldIsFromLocalClass;
   }

TR_FieldAttributesCache *
TR_ResolvedRelocatableJ9JITaaSServerMethod::getAttributesCache(bool isStatic, bool unresolvedInCP)
   {
   TR::CompilationInfoPerThread *compInfoPT = _fe->_compInfoPT;
   auto &attributesCache = isStatic ? 
      getJ9ClassInfo(compInfoPT, _ramClass)._staticAttributesCacheAOT :
      getJ9ClassInfo(compInfoPT, _ramClass)._fieldAttributesCacheAOT;
   if (!attributesCache)
      {
      // initialize cache, called once per ram class
      attributesCache = new (PERSISTENT_NEW) TR_FieldAttributesCache(TR_FieldAttributesCache::allocator_type(TR::Compiler->persistentAllocator()));
      }
   return attributesCache;
   }
