#pragma once
///@file

#include "nix/util/serialise.hh"

#include <variant>

namespace nix {

struct StoreDirConfig;
struct Source;

// items being serialized
class StorePath;

/**
 * Shared serializers between the worker protocol, serve protocol, and a
 * few others.
 *
 * This `struct` is basically just a `namespace`; We use a type rather
 * than a namespace just so we can use it as a template argument.
 */
struct CommonProto
{
    /**
     * A unidirectional read connection, to be used by the read half of the
     * canonical serializers below.
     */
    struct ReadConn
    {
        Source & from;
    };

    /**
     * A unidirectional write connection, to be used by the write half of the
     * canonical serializers below.
     */
    struct WriteConn
    {
        Sink & to;
    };

    template<typename T>
    struct Serialise;

    /**
     * Wrapper function around `CommonProto::Serialise<T>::write` that allows us to
     * infer the type instead of having to write it down explicitly.
     */
    template<typename T>
    static void write(const StoreDirConfig & store, WriteConn conn, const T & t)
    {
        CommonProto::Serialise<T>::write(store, conn, t);
    }
};

#define DECLARE_COMMON_SERIALISER(T)                                                                 \
    struct CommonProto::Serialise<T>                                                                 \
    {                                                                                                \
        static T read(const StoreDirConfig & store, CommonProto::ReadConn conn);                     \
        static void write(const StoreDirConfig & store, CommonProto::WriteConn conn, const T & str); \
    }

template<>
DECLARE_COMMON_SERIALISER(StorePath);

#define COMMA_ ,
template<typename T>
DECLARE_COMMON_SERIALISER(std::vector<T>);
template<typename T, typename Compare>
DECLARE_COMMON_SERIALISER(std::set<T COMMA_ Compare>);
template<typename... Ts>
DECLARE_COMMON_SERIALISER(std::tuple<Ts...>);

template<typename K, typename V, typename Compare>
DECLARE_COMMON_SERIALISER(std::map<K COMMA_ V COMMA_ Compare>);
#undef COMMA_

} // namespace nix
