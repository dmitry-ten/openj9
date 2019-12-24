
#ifndef RAW_TYPE_CONVERT_H
#define RAW_TYPE_CONVERT_H

#include <cstdint>
#include <utility>
#include <type_traits>
#include "StreamExceptions.hpp"
#include "omrcomp.h"
#include "net/Message.hpp"

class J9Class;
class TR_ResolvedJ9Method;

namespace JITServer
   {
   // Things required for converting data passed to type converter into
   // a format that we can simply write into the socket and vice versa.
   // 1. Receive tuple of data points in stream::write
   // 2. For every data point, convert it to a format acceptable by socket write
   //   - For primitive and trivially copyable data types no conversion needed
   //   - For strings, send the buffer containing chars
   //   - For vectors, send the buffer containing elements
   //   - For tuples, convert tuple to a vector and then send the vector
   //   This should cover all currently supported cases of data structures.
   // 3. Write message type to the socket
   // 4. Write the number of data points sent to the socket
   // 5. Before writing every data point, write metadata that should describe
   // the type of the data and its size.
   // 6. Write every serialized data point to the socket.
   //
   // On the receiving side:
   // 1. Read message type.
   // 2. Read number of data points
   // 3. For every data point, first read the metadata
   // 4. Read data point and convert it to its original data type.
   //   - for primitives and trivially copyable types we just cast to the type
   //   - for vectors, will need to allocate vector filled with received values
   //   - for tuples will need to create a tuple filled with received values
   // 5. Create a tuple of all deserialized data points and return it.
   //
   // enum MessageType {
      // compilationCode = 0,
      // compilationFailure = 1
   // };



   template <typename T, typename = void> struct RawTypeConvert { };
   // For primitive values, just return pointer to them, since they can be written/read directly
   // template <typename T> struct RawTypeConvert<T, typename std::enable_if<std::is_fundamental<T>::value>::type>
      // {
      // static inline T onRecv(const Message::DataPoint *dataPoint) { return (T) dataPoint->data; }
      // static inline Message::DataPoint onSend(const T &value)
         // {
         // Message::DataPoint dPoint = { Message:typeName(value), sizeof(T), &value};
         // return dPoint;
         // }
      // };
   template <> struct RawTypeConvert<const std::string>
      {
      static inline std::string& onRecv(const Message::DataPoint *dataPoint)
         {
         return *reinterpret_cast<std::string *>(dataPoint->data);
         }
      };

   // For trivially copyable classes
   template <typename T> struct RawTypeConvert<T, typename std::enable_if<std::is_trivially_copyable<T>::value>::type>
      {
      static inline T onRecv(const Message::DataPoint *dataPoint) { return (T) dataPoint->data; }
      static inline Message::DataPoint onSend(const T &value)
         {
         Message::DataPoint dPoint = { Message::DataType::OBJECT, sizeof(T), &value };
         return dPoint;
         }
      };

   // For vectors
   template <typename T> struct RawTypeConvert<T, typename std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type>
      {
      static inline T onRecv(const Message::DataPoint *dataPoint)
         {
         auto arrayStart = reinterpret_cast<typename T::value_type *>(dataPoint->data); 
         std::vector<typename T::value_type> out(arrayStart, arrayStart + dataPoint->metaData.size / sizeof(T::value_type));
         return out;
         }
      static inline void *onSend(const T &value)
         {
         Message::DataPoint dPoint = { Message::DataType::VECTOR, sizeof(T) * value.size(), NULL };
         if (value.size() > 0)
            {
            // call onSend for every value of the vector and put the result in an array;
            // Or maybe not. If we send only trivially copyable values in the vector, we should be fine.
            dPoint.data = &value[0];
            return dPoint;
            }
         return NULL;
         }
      };
   // For tuples
   //

   // setArgs fills out a message with values from a variadic argument list.
   // calls RawTypeConvert::onSend for each argument.
   template <typename Arg1, typename... Args>
   struct SetArgsRaw
      {
      static void setArgs(Message &message, Arg1 arg1, Args... args)
         {
         SetArgsRaw<Arg1>::setArgs(message, arg1);
         SetArgsRaw<Args...>::setArgs(message, args...);
         }
      };
   template <typename Arg1>
   struct SetArgsRaw<Arg1>
      {
      static void setArgs(Message &message, Arg1 arg1)
         {
         Message::DataPoint dPoint = RawTypeConvert<Arg1>::onSend(arg1);
         message.addDataPoint(dPoint);
         }
      };
   template <typename... Args>
   void setArgsRaw(Message &message, Args... args)
      {
      message.clear();
      SetArgsRaw<Args...>::setArgs(message, args...);
      }

   // getArgs returns a tuple of values which are extracted from a protobuf AnyData message.
   // It calls ProtobufTypeConvert::onRecv for each value extracted.
   template <typename Arg1, typename... Args>
   struct GetArgsRaw
      {
      static std::tuple<Arg1, Args...> getArgs(const Message &message, size_t n)
         {
         return std::tuple_cat(GetArgsRaw<Arg1>::getArgs(message, n), GetArgsRaw<Args...>::getArgs(message, n + 1));
         }
      };
   template <typename Arg>
   struct GetArgsRaw<Arg>
      {
      static std::tuple<Arg> getArgs(const Message &message, size_t n)
         {
         auto data = message.getDataPoint(n);
         // if (data.type_case() != AnyPrimitive<typename ProtobufTypeConvert<Arg>::ProtoType>::typeCase())
            // throw StreamTypeMismatch("Received type " + std::to_string(data.type_case()) + " but expect type " + std::to_string(AnyPrimitive<typename ProtobufTypeConvert<Arg>::ProtoType>::typeCase()));
         return std::make_tuple(RawTypeConvert<Arg>::onRecv(&data));
         }
      };
   template <typename... Args>
   std::tuple<Args...> getArgsRaw(const Message &message)
      {
      if (sizeof...(Args) != message.getMetaData().numDataPoints)
         throw StreamArityMismatch("Received " + std::to_string(message.getMetaData().numDataPoints) + " args to unpack but expect " + std::to_string(sizeof...(Args)) + "-tuple");
      return GetArgsRaw<Args...>::getArgs(message, 0);
      }
   };
#endif
