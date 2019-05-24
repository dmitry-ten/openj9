
/*******************************************************************************
 * Copyright (c) 2000, 2019 IBM Corp. and others
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

#include "rpc/gen/compile.pb.h"

#ifndef PROTOBUF_WRAPPERS_H
#define PROTOBUF_WRAPPERS_H

#include "j9.h"
#include "env/jittypes.h"
#include "codegen/RecognizedMethods.hpp"
// #include "env/PersistentCollections.hpp"


using namespace google::protobuf;
class TR_ResolvedJ9Method;

namespace JITaaS
{
struct
TR_ResolvedMethodInfoWrapper
   {
   TR_ResolvedMethodInfoWrapper()
      {
      }

   TR_ResolvedMethodInfoWrapper(const ResolvedMethodInfo &serializedInfo)
      {
      deserialize(serializedInfo);
      }

   void deserialize(const ResolvedMethodInfo &serializedInfo)
      {
      remoteMirror = (TR_ResolvedJ9Method *) serializedInfo.remotemirror();
      literals = (J9RAMConstantPoolItem *) serializedInfo.literals();
      ramClass = (J9Class *) serializedInfo.ramclass();
      methodIndex = serializedInfo.methodindex();
      jniProperties = (uintptrj_t) serializedInfo.jniproperties();
      jniTargetAddress = (void *) serializedInfo.jnitargetaddress();
      isInterpreted = serializedInfo.isinterpreted();
      isJNINative = serializedInfo.isjninative();
      isMethodInValidLibrary = serializedInfo.ismethodinvalidlibrary();
      mandatoryRm = (TR::RecognizedMethod) serializedInfo.mandatoryrm();
      rm = (TR::RecognizedMethod) serializedInfo.rm();
      startAddressForJittedMethod = (void *) serializedInfo.startaddressforjittedmethod();
      virtualMethodIsOverridden = serializedInfo.virtualmethodisoverridden();
      addressContainingIsOverriddenBit = (void *) serializedInfo.addresscontainingisoverriddenbit();
      classLoader = (J9ClassLoader *) serializedInfo.classloader();
      bodyInfoStr = serializedInfo.bodyinfostr();
      methodInfoStr = serializedInfo.methodinfostr();
      entryStr = serializedInfo.entrystr();
      }

   void serialize(ResolvedMethodInfo *serializedInfo) const
      {
      serializedInfo->set_remotemirror((uint64) remoteMirror);
      serializedInfo->set_literals((uint64) literals);
      serializedInfo->set_ramclass((uint64) ramClass);
      serializedInfo->set_methodindex(methodIndex);
      serializedInfo->set_jniproperties((uint64) jniProperties);
      serializedInfo->set_jnitargetaddress((uint64) jniTargetAddress);
      serializedInfo->set_isinterpreted(isInterpreted);
      serializedInfo->set_isjninative(isJNINative);
      serializedInfo->set_ismethodinvalidlibrary(isMethodInValidLibrary);
      serializedInfo->set_mandatoryrm((int32) mandatoryRm);
      serializedInfo->set_rm((int32) rm);
      serializedInfo->set_startaddressforjittedmethod((uint64) startAddressForJittedMethod);
      serializedInfo->set_virtualmethodisoverridden(virtualMethodIsOverridden);
      serializedInfo->set_addresscontainingisoverriddenbit((uint64) addressContainingIsOverriddenBit);
      serializedInfo->set_classloader((uint64) classLoader);
      serializedInfo->set_bodyinfostr(bodyInfoStr);
      serializedInfo->set_methodinfostr(methodInfoStr);
      serializedInfo->set_entrystr(entryStr);
      }

   TR_ResolvedJ9Method *remoteMirror;
   J9RAMConstantPoolItem *literals;
   J9Class *ramClass;
   uint64_t methodIndex;
   uintptrj_t jniProperties;
   void *jniTargetAddress;
   bool isInterpreted;
   bool isJNINative;
   bool isMethodInValidLibrary;
   TR::RecognizedMethod mandatoryRm;
   TR::RecognizedMethod rm;
   void *startAddressForJittedMethod;
   bool virtualMethodIsOverridden;
   void *addressContainingIsOverriddenBit;
   J9ClassLoader *classLoader;
   std::string bodyInfoStr;
   std::string methodInfoStr;
   std::string entryStr;
   };

struct
TR_ClassInfoWrapper
   {
   TR_ClassInfoWrapper()
      {
      }
   TR_ClassInfoWrapper(const RAMClassInfo &serializedInfo)
      {
      deserialize(serializedInfo);
      }

   void deserialize(const RAMClassInfo &serializedInfo)
      {
      romClassStr = serializedInfo.romclassstr();
      remoteRomClass = (J9ROMClass *) serializedInfo.remoteromclass();
      methodsOfClass = (J9Method *) serializedInfo.methodsofclass();
      baseComponentClass = (TR_OpaqueClassBlock *) serializedInfo.basecomponentclass();
      numDimensions = (int32_t) serializedInfo.numdimensions();
      // methodTracingInfo = 
      parentClass = (TR_OpaqueClassBlock *) serializedInfo.parentclass();
      // interfaces = 
      classHasFinalFields = serializedInfo.classhasfinalfields();
      classDepthAndFlags = (uintptrj_t) serializedInfo.classdepthandflags();
      classInitialized = serializedInfo.classinitialized();
      byteOffsetToLockword = serializedInfo.byteoffsettolockword();
      leafComponentClass = (TR_OpaqueClassBlock *) serializedInfo.leafcomponentclass();
      classLoader = (void *) serializedInfo.classloader();
      hostClass = (TR_OpaqueClassBlock *) serializedInfo.hostclass();
      componentClass = (TR_OpaqueClassBlock *) serializedInfo.componentclass();
      arrayClass = (TR_OpaqueClassBlock *) serializedInfo.arrayclass();
      totalInstanceSize = (uintptrj_t) serializedInfo.totalinstancesize();
      constantPool = (J9ConstantPool *) serializedInfo.constantpool();
      }

   void serialize(RAMClassInfo *serializedInfo) const
      {
      serializedInfo->set_romclassstr(romClassStr);
      serializedInfo->set_remoteromclass((uint64) remoteRomClass);
      serializedInfo->set_methodsofclass((uint64) methodsOfClass);
      serializedInfo->set_basecomponentclass((uint64) baseComponentClass);
      serializedInfo->set_numdimensions((int32) numDimensions);
      // serializedInfo->set_methodtracinginfo();
      serializedInfo->set_parentclass((uint64) parentClass);
      // serializedInfo->set_interfaces();
      serializedInfo->set_classhasfinalfields(classHasFinalFields);
      serializedInfo->set_classdepthandflags((uint64) classDepthAndFlags);
      serializedInfo->set_classinitialized(classInitialized);
      serializedInfo->set_byteoffsettolockword((uint32) byteOffsetToLockword);
      serializedInfo->set_leafcomponentclass((uint64) leafComponentClass);
      serializedInfo->set_classloader((uint64) classLoader);
      serializedInfo->set_hostclass((uint64) hostClass);
      serializedInfo->set_componentclass((uint64) componentClass);
      serializedInfo->set_arrayclass((uint64) arrayClass);
      serializedInfo->set_totalinstancesize((uint64) totalInstanceSize);
      serializedInfo->set_constantpool((uint64) constantPool);
      }

   // attributes passed from the client
   std::string romClassStr;
   J9ROMClass *remoteRomClass;
   J9Method *methodsOfClass;
   TR_OpaqueClassBlock *baseComponentClass;
   int32_t numDimensions;
   std::vector<bool> methodTracingInfo;
   TR_OpaqueClassBlock *parentClass;
   std::vector<TR_OpaqueClassBlock *> interfaces;
   bool classHasFinalFields;
   uintptrj_t classDepthAndFlags;
   bool classInitialized;
   uint32_t byteOffsetToLockword;
   TR_OpaqueClassBlock *leafComponentClass;
   void *classLoader;
   TR_OpaqueClassBlock *hostClass;
   TR_OpaqueClassBlock *componentClass;
   TR_OpaqueClassBlock *arrayClass;
   uintptrj_t totalInstanceSize;
   J9ConstantPool *constantPool;

   // parameters set by the server
   J9ROMClass *romClass;
   PersistentUnorderedMap<TR_RemoteROMStringKey, std::string> *_remoteROMStringsCache;
   PersistentUnorderedMap<int32_t, std::string> *_fieldOrStaticNameCache;
   PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *> *_classOfStaticCache;
   PersistentUnorderedMap<int32_t, TR_OpaqueClassBlock *> *_constantClassPoolCache;
   TR_FieldAttributesCache *_fieldAttributesCache;
   TR_FieldAttributesCache *_staticAttributesCache;
   TR_FieldAttributesCache *_fieldAttributesCacheAOT;
   TR_FieldAttributesCache *_staticAttributesCacheAOT;
   };
}
#endif // PROTOBUF_WRAPPERS_H
