
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

   template <typename T, typename = void> struct RawTypeConvert { };
   template <> struct RawTypeConvert<uint32_t>
      {
      static inline uint32_t onRecv(const Message::DataPoint *dPoint) { return *static_cast<uint32_t *>(dPoint->data); }
      static inline Message::DataPoint onSend(const uint32_t &val)
         {
         return Message::DataPoint(Message::DataType::UINT32, sizeof(uint32_t), &val);
         }
      };
   template <> struct RawTypeConvert<uint64_t>
      {
      static inline uint64_t onRecv(const Message::DataPoint *dPoint) { return *static_cast<uint64_t *>(dPoint->data); }
      static inline Message::DataPoint onSend(const uint64_t &val)
         {
         return Message::DataPoint(Message::DataType::UINT64, sizeof(uint64_t), &val);
         }
      };
   template <> struct RawTypeConvert<int32_t>
      {
      static inline int32_t onRecv(const Message::DataPoint *dPoint) { return *static_cast<int32_t *>(dPoint->data); }
      static inline Message::DataPoint onSend(const int32_t &val)
         {
         return Message::DataPoint(Message::DataType::INT32, sizeof(int32_t), &val);
         }
      };
   template <> struct RawTypeConvert<int64_t>
      {
      static inline int64_t onRecv(const Message::DataPoint *dPoint) { return *static_cast<int64_t *>(dPoint->data); }
      static inline Message::DataPoint onSend(const int64_t &val)
         {
         return Message::DataPoint(Message::DataType::INT64, sizeof(int64_t), &val);
         }
      };
   template <> struct RawTypeConvert<bool>
      {
      static inline bool onRecv(const Message::DataPoint *dPoint) { return *static_cast<bool *>(dPoint->data); }
      static inline Message::DataPoint onSend(const bool &val)
         {
         return Message::DataPoint(Message::DataType::BOOL, sizeof(bool), &val);
         }
      };
   template <> struct RawTypeConvert<const std::string>
      {
      static inline std::string onRecv(const Message::DataPoint *dataPoint)
         {
         return std::string(static_cast<char *>(dataPoint->data), dataPoint->metaData.size);
         }
      static inline Message::DataPoint onSend(const std::string &value)
         {
         return Message::DataPoint(Message::DataType::STRING, value.length(), &value[0]);
         }
      };

   template <> struct RawTypeConvert<std::string> : RawTypeConvert<const std::string> {};

   // For trivially copyable classes
   template <typename T> struct RawTypeConvert<T, typename std::enable_if<std::is_trivially_copyable<T>::value>::type>
      {
      static inline T onRecv(const Message::DataPoint *dataPoint) { return *static_cast<T *>(dataPoint->data); }
      static inline Message::DataPoint onSend(const T &value)
         {
         return Message::DataPoint(Message::DataType::OBJECT, sizeof(T), &value);
         }
      };

   // For vectors
   template <typename T> struct RawTypeConvert<T, typename std::enable_if<std::is_same<T, std::vector<typename T::value_type>>::value>::type>
      {
      static inline T onRecv(const Message::DataPoint *dataPoint)
         {
         uint32_t size = *static_cast<uint32_t *>(dataPoint->data);

         std::vector<typename T::value_type> values;
         values.reserve(size);

         auto dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(dataPoint->data) + 1);
         for (int32_t i = 0; i < size; ++i)
            {
            values.push_back(RawTypeConvert<typename T::value_type>::onRecv(&dPoints[i]));
            }
         return values;
         }
      static inline Message::DataPoint onSend(const T &value)
         {
         uint32_t dataSize = sizeof(uint32_t) + value.size() * sizeof(Message::DataPoint);
         Message::DataPoint dPoint = { Message::DataType::VECTOR, dataSize, NULL };
         // Serialize each element in a vector as its own datapoint
         // call onSend for every value of the vector and put the result in a dynamically allocated buffer
         void *storage = malloc(dataSize);
         *static_cast<uint32_t *>(storage) = value.size();
         auto dPoints = reinterpret_cast<Message::DataPoint *>(static_cast<uint32_t *>(storage) + 1);
         for (int32_t i = 0; i < value.size(); ++i)
            {
            dPoints[i] = RawTypeConvert<typename T::value_type>::onSend(value[i]);
            }
         dPoint.data = storage;
         return dPoint;
         }
      };
   // For tuples
   //
   // used to unpack a tuple into variadic args
   template <size_t... I> struct index_tuple_raw { template <size_t N> using type = index_tuple_raw<I..., N>; };
   template <size_t N> struct index_tuple_gen_raw { using type = typename index_tuple_gen_raw<N - 1>::type::template type<N - 1>; };
   template <> struct index_tuple_gen_raw<0> { using type = index_tuple_raw<>; };

   template <size_t n, typename Arg1, typename... Args>
   struct TupleTypeConvert
      {
      static inline std::tuple<Arg1, Args...> onRecvImpl(void *startPtr)
         {
         return std::tuple_cat(TupleTypeConvert<n, Arg1>::onRecvImpl(startPtr),
                               TupleTypeConvert<n + 1, Args...>::onRecvImpl(static_cast<void *>(static_cast<Message::DataPoint *>(startPtr) + 1)));
         }
      static inline void onSendImpl(void *startPtr, const Arg1 &arg1, const Args&... args)
         {
         TupleTypeConvert<n, Arg1>::onSendImpl(startPtr, arg1);
         TupleTypeConvert<n + 1, Args...>::onSendImpl(static_cast<void *>(static_cast<Message::DataPoint *>(startPtr) + 1), args...);
         }
      };

   template <size_t n, typename Arg1>
   struct TupleTypeConvert<n, Arg1>
      {
      static inline std::tuple<Arg1> onRecvImpl(void *startPtr)
         {
         Message::DataPoint *dPoint = reinterpret_cast<Message::DataPoint *>(startPtr);
         return std::make_tuple(RawTypeConvert<Arg1>::onRecv(dPoint));
         }
      static inline void onSendImpl(void *startPtr, const Arg1 &arg1)
         {
         *static_cast<Message::DataPoint *>(startPtr) = RawTypeConvert<Arg1>::onSend(arg1);
         }
      };

   template <typename... T> struct RawTypeConvert<const std::tuple<T...>>
      {
      static inline std::tuple<T...> onRecv(const Message::DataPoint *dataPoint)
         {
         return TupleTypeConvert<0, T...>::onRecvImpl(static_cast<void *>((static_cast<uint32_t *>(dataPoint->data) + 1)));
         }

      template <typename Tuple, size_t... Idx>
      static inline Message::DataPoint onSendImpl(const Tuple &val, index_tuple_raw<Idx...>)
         {
         size_t tupleSize = std::tuple_size<typename std::decay<decltype(val)>::type>::value;
         uint32_t dataSize = sizeof(uint32_t) + tupleSize * sizeof(Message::DataPoint);
         Message::DataPoint dPoint = { Message::DataType::TUPLE, dataSize, NULL };
         void *storage = malloc(dataSize);
         *static_cast<uint32_t *>(storage) = tupleSize;
         TupleTypeConvert<0, T...>::onSendImpl(static_cast<void *>(static_cast<uint32_t *>(storage) + 1), std::get<Idx>(val)...);
         dPoint.data = storage;
         return dPoint;
         }
      
      static inline Message::DataPoint onSend(const std::tuple<T...> &val)
         {
         // Serialize each element in a tuple as its own datapoint
         // call onSend for every value of the tuple and put the result in a newly allocated buffer;
         using Idx = typename index_tuple_gen_raw<sizeof...(T)>::type;
         return onSendImpl(val, Idx());
         }
      };

   template <typename... T> struct RawTypeConvert<std::tuple<T...>> : RawTypeConvert<const std::tuple<T...>> { };

   // setArgs fills out a message with values from a variadic argument list.
   // calls RawTypeConvert::onSend for each argument.
   template <typename Arg1, typename... Args>
   struct SetArgsRaw
      {
      static void setArgs(Message &message, Arg1 &arg1, Args&... args)
         {
         SetArgsRaw<Arg1>::setArgs(message, arg1);
         SetArgsRaw<Args...>::setArgs(message, args...);
         }
      };
   template <typename Arg1>
   struct SetArgsRaw<Arg1>
      {
      static void setArgs(Message &message, Arg1 &arg1)
         {
         Message::DataPoint dPoint = RawTypeConvert<Arg1>::onSend(arg1);
         message.addDataPoint(dPoint, true);
         }
      };
   template <typename... Args>
   void setArgsRaw(Message &message, Args&... args)
      {
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
